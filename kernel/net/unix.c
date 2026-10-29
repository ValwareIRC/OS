#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>
#include "sock.h"

/* AF_UNIX sockets: the local IPC Wayland, D-Bus and most desktop software
 * are built on.
 *
 * Each socket has a receive queue of segments. A sender appends to its
 * peer's queue. A segment may carry open files (SCM_RIGHTS), which travel
 * with its first byte: a stream read never continues past a segment that
 * carries files, so they arrive together with the bytes they were sent with.
 * Seqpacket and datagram sockets read one segment per call.
 *
 * All AF_UNIX state is guarded by one lock. The traffic is local and short,
 * and a single lock keeps peer pointers, listen queues and names consistent
 * without lock-ordering rules. Files are never released while it is held:
 * releasing a socket file comes back here. Peers hold no references to each
 * other; closing a socket clears every pointer to it under the lock. */

#define UNIX_BUF (256 * 1024) /* receive queue limit per socket */
#define UNIX_MAX_FDS 253      /* SCM_MAX_FD */
#define UNIX_PATH_MAX 108
#define UNIX_MAX_CONTROL 8192
#define UNIX_CHUNK 65536

struct uaddr {
    uint32_t len; /* bytes of sun_path: strlen + 1 for paths, the whole name for abstract ones */
    char path[UNIX_PATH_MAX + 1];
};

struct useg {
    struct useg *next;
    size_t off, len, cap; /* unread bytes are data[off, len) */
    struct file **fds;
    int nfds;
    struct uaddr *from; /* datagrams: the sender's name, if it has one */
    struct linux_ucred cred; /* who sent it (global ids), for SCM_CREDENTIALS */
    uint8_t data[];
};

enum ustate { US_UNCONNECTED, US_LISTENING, US_CONNECTED };

struct usock {
    int type; /* SOCK_STREAM, SOCK_DGRAM or SOCK_SEQPACKET */
    enum ustate state;
    struct usock *peer;
    bool rcv_shut, snd_shut; /* nothing more will arrive / may be sent */
    bool dgram_connected;    /* datagram socket with a default destination */
    bool bound;              /* has a name of its own (bind or autobind) */
    bool passcred;
    struct waitq wq; /* readers, writers, accepters and pollers */

    struct useg *rx_head, *rx_tail;
    size_t rx_bytes;

    struct uaddr name, peer_name;
    struct vnode *node;            /* filesystem entry, if bound to a path */
    struct linux_ucred cred;       /* ours, as handed to peers (taken at socket() and listen()) */
    struct linux_ucred peer_cred;  /* SO_PEERCRED */
    struct netns *netns;           /* abstract names are per network namespace (referenced) */

    /* listening: connections waiting for accept() */
    struct usock *pending_head, *pending_tail, *next_pending;
    int npending, backlog;

    struct usock *next_all;
};

static spinlock_t unix_lock = SPINLOCK_INIT;
static struct usock *all_usocks;
static uint32_t autobind_next;

static const struct vnode_ops unix_ops, unix_node_ops;

bool unix_is_socket(struct file *f) { return f->vnode->ops == &unix_ops; }

static void wake(struct usock *s) {
    if (s)
        waitq_wake_all(&s->wq);
}

static void current_cred(struct linux_ucred *c) {
    struct process *p = proc_current();
    *c = (struct linux_ucred){p->pid, p->euid, p->egid};
}

static size_t room(struct usock *s) { return s->rx_bytes < UNIX_BUF ? UNIX_BUF - s->rx_bytes : 0; }

static bool addr_eq(const struct uaddr *a, const struct uaddr *b) {
    return a->len == b->len && !memcmp(a->path, b->path, a->len);
}

static struct usock *us_new(int type) {
    struct usock *s = kzalloc(sizeof *s);
    if (!s)
        return NULL;
    s->type = type;
    waitq_init(&s->wq);
    current_cred(&s->cred);
    s->peer_cred = (struct linux_ucred){0, (uint32_t)-1, (uint32_t)-1};
    s->netns = netns_get(netns_current());
    return s;
}

/* Lock held. */
static void us_link(struct usock *s) {
    s->next_all = all_usocks;
    all_usocks = s;
}

/* ---- teardown ---- */

/* What a teardown leaves to do once the lock is dropped: segments may hold
 * files, and file_put may come back into this code. */
struct reap {
    struct useg *segs;
    struct usock *socks;
};

static void free_segs(struct useg *g) {
    while (g) {
        struct useg *next = g->next;
        for (int i = 0; i < g->nfds; i++)
            file_put(g->fds[i]);
        kfree(g->fds);
        kfree(g->from);
        kfree(g);
        g = next;
    }
}

/* Lock held. Unhooks `s` from everything that points at it. */
static void us_destroy_locked(struct usock *s, struct reap *r) {
    for (struct usock **pp = &all_usocks; *pp; pp = &(*pp)->next_all)
        if (*pp == s) {
            *pp = s->next_all;
            break;
        }
    if (s->node)
        s->node->data = NULL; /* the name stays in the file system, refusing connections */
    struct usock *peer = s->peer;
    if (peer && peer->peer == s) {
        peer->peer = NULL;
        if (peer->type != SOCK_DGRAM)
            peer->rcv_shut = peer->snd_shut = true; /* reads drain, then EOF; writes EPIPE */
        wake(peer);
    }
    for (struct usock *o = all_usocks; o; o = o->next_all) /* datagram sockets connected to us */
        if (o->peer == s) {
            o->peer = NULL;
            wake(o);
        }
    while (s->pending_head) { /* connections nobody accepted */
        struct usock *e = s->pending_head;
        s->pending_head = e->next_pending;
        us_destroy_locked(e, r);
    }
    if (s->rx_tail) {
        s->rx_tail->next = r->segs;
        r->segs = s->rx_head;
    }
    s->rx_head = s->rx_tail = NULL;
    s->next_all = r->socks;
    r->socks = s;
}

static void reap_finish(struct reap *r) {
    free_segs(r->segs);
    while (r->socks) {
        struct usock *s = r->socks;
        r->socks = s->next_all;
        if (s->node)
            vnode_put(s->node);
        netns_put(s->netns);
        kfree(s);
    }
}

static void us_release(struct vnode *v) {
    struct usock *s = v->data;
    if (!s)
        return;
    struct reap r = {NULL, NULL};
    uint64_t f = spin_lock_irqsave(&unix_lock);
    us_destroy_locked(s, &r);
    spin_unlock_irqrestore(&unix_lock, f);
    reap_finish(&r);
}

/* Gives socket `s` (already linked) a file and a descriptor. On failure the
 * socket is torn down. */
static int us_install(struct usock *s, uint64_t sflags) {
    struct vnode *v = vnode_alloc(VN_SOCK, 0777, &unix_ops);
    if (!v) {
        struct reap r = {NULL, NULL};
        uint64_t f = spin_lock_irqsave(&unix_lock);
        us_destroy_locked(s, &r);
        spin_unlock_irqrestore(&unix_lock, f);
        reap_finish(&r);
        return -ENOMEM;
    }
    v->data = s;
    return vnode_install_fd(v, O_RDWR | ((sflags & SOCK_NONBLOCK) ? O_NONBLOCK : 0), sflags & SOCK_CLOEXEC);
}

/* ---- names ---- */

static int read_uaddr(uint64_t uaddr, uint64_t len, struct uaddr *a) {
    uint8_t buf[2 + UNIX_PATH_MAX];
    if (len < 2 || len > sizeof buf)
        return -EINVAL;
    if (copy_from_user(buf, uaddr, len) < 0)
        return -EFAULT;
    if ((buf[0] | buf[1] << 8) != AF_UNIX)
        return -EINVAL;
    size_t plen = len - 2;
    memset(a, 0, sizeof *a);
    if (plen && buf[2] == 0) { /* abstract: every byte counts, NULs included */
        memcpy(a->path, buf + 2, plen);
        a->len = (uint32_t)plen;
    } else if (plen) {
        size_t n = 0;
        while (n < plen && buf[2 + n])
            n++;
        memcpy(a->path, buf + 2, n);
        a->len = (uint32_t)n + 1;
    }
    return 0;
}

static int write_uaddr(uint64_t uaddr, uint64_t ulen, const struct uaddr *a) {
    if (!uaddr)
        return 0;
    uint32_t len;
    if (copy_from_user(&len, ulen, sizeof len) < 0)
        return -EFAULT;
    if ((int32_t)len < 0)
        return -EINVAL;
    uint8_t buf[2 + UNIX_PATH_MAX + 1] = {AF_UNIX, 0};
    memcpy(buf + 2, a->path, a->len);
    uint32_t full = 2 + a->len;
    if (copy_to_user(uaddr, buf, len < full ? len : full) < 0)
        return -EFAULT;
    return copy_to_user(ulen, &full, sizeof full);
}

static bool is_abstract(const struct uaddr *a) { return a->len && a->path[0] == 0; }

/* Lock held. The socket bound to `a`; `node` is its file system entry for
 * path names (looked up beforehand, outside the lock). */
static struct usock *resolve_locked(const struct uaddr *a, struct vnode *node) {
    if (!is_abstract(a))
        return node ? node->data : NULL;
    struct netns *here = netns_current();
    for (struct usock *o = all_usocks; o; o = o->next_all)
        if (o->bound && !o->node && o->netns == here && addr_eq(&o->name, a))
            return o;
    return NULL;
}

/* Looks up the file system entry for a path name. NULL with 0 for abstract
 * names. */
static int lookup_node(const struct uaddr *a, struct vnode **out) {
    *out = NULL;
    if (is_abstract(a))
        return 0;
    struct vnode *v;
    int r = vfs_lookup(NULL, a->path, LOOKUP_FOLLOW, &v);
    if (r < 0)
        return r;
    if (v->ops != &unix_node_ops) {
        vnode_put(v);
        return -ECONNREFUSED;
    }
    *out = v;
    return 0;
}

static int node_open(struct vnode *v, struct file *f) {
    (void)v, (void)f;
    return -ENXIO; /* sockets are reached with connect(), not open() */
}

static const struct vnode_ops unix_node_ops = {.open = node_open};

/* Lock held. A unique abstract name, as Linux gives on autobind. */
static void autobind_locked(struct usock *s) {
    for (;;) {
        struct uaddr a = {.len = 6};
        ksnprintf(a.path + 1, sizeof a.path - 1, "%05x", autobind_next++ & 0xfffff);
        if (!resolve_locked(&a, NULL)) {
            s->name = a;
            s->bound = true;
            return;
        }
    }
}

int64_t unix_bind(struct file *f, uint64_t uaddr, uint64_t len) {
    struct usock *s = f->vnode->data;
    struct uaddr a;
    int r = read_uaddr(uaddr, len, &a);
    if (r < 0)
        return r;
    uint64_t fl;
    if (!a.len || is_abstract(&a)) {
        fl = spin_lock_irqsave(&unix_lock);
        if (s->bound)
            r = -EINVAL;
        else if (!a.len)
            autobind_locked(s);
        else if (resolve_locked(&a, NULL))
            r = -EADDRINUSE;
        else {
            s->name = a;
            s->bound = true;
        }
        spin_unlock_irqrestore(&unix_lock, fl);
        return r;
    }
    if (s->bound)
        return -EINVAL;
    /* A path name: a socket entry in the directory (ramfs). */
    struct vnode *dir;
    char name[256];
    r = vfs_lookup_parent(NULL, a.path, &dir, name, sizeof name);
    if (r < 0)
        return r == -EEXIST ? -EADDRINUSE : r;
    struct vnode *node = vnode_alloc(VN_SOCK, 0777 & ~proc_current()->umask, &unix_node_ops);
    if (!node) {
        vnode_put(dir);
        return -ENOMEM;
    }
    node->data = s;
    r = dir->ops->create ? ramfs_add_node(dir, name, node) : -EROFS;
    vnode_put(dir);
    if (r < 0) {
        vnode_put(node);
        return r == -EEXIST ? -EADDRINUSE : r;
    }
    fl = spin_lock_irqsave(&unix_lock);
    s->node = node; /* our reference from vnode_alloc */
    s->name = a;
    s->bound = true;
    spin_unlock_irqrestore(&unix_lock, fl);
    return 0;
}

int64_t unix_listen(struct file *f, int backlog) {
    struct usock *s = f->vnode->data;
    int64_t r = 0;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    if (s->type == SOCK_DGRAM)
        r = -EOPNOTSUPP;
    else if (!s->bound || s->state == US_CONNECTED)
        r = -EINVAL;
    else {
        s->state = US_LISTENING;
        s->backlog = backlog < 1 ? 1 : backlog > 128 ? 128 : backlog;
        current_cred(&s->cred);
    }
    spin_unlock_irqrestore(&unix_lock, fl);
    return r;
}

int64_t unix_connect(struct file *f, uint64_t uaddr, uint64_t len) {
    struct usock *s = f->vnode->data;
    struct uaddr a;
    int r = read_uaddr(uaddr, len, &a);
    if (r < 0)
        return r;
    if (!a.len)
        return -EINVAL;
    for (;;) {
        struct vnode *node;
        r = lookup_node(&a, &node);
        if (r < 0)
            return r;
        /* The server's end of the new connection, made before taking the lock. */
        struct usock *e = s->type != SOCK_DGRAM ? us_new(s->type) : NULL;
        if (s->type != SOCK_DGRAM && !e) {
            if (node)
                vnode_put(node);
            return -ENOMEM;
        }
        bool full = false;
        uint64_t fl = spin_lock_irqsave(&unix_lock);
        struct usock *l = resolve_locked(&a, node);
        if (!l)
            r = -ECONNREFUSED;
        else if (l->type != s->type)
            r = -EPROTOTYPE;
        else if (s->type == SOCK_DGRAM) {
            s->peer = l;
            s->dgram_connected = true;
            s->peer_name = l->name;
            s->peer_cred = l->cred;
        } else if (s->state == US_CONNECTED)
            r = -EISCONN;
        else if (s->state == US_LISTENING)
            r = -EINVAL;
        else if (l->state != US_LISTENING)
            r = -ECONNREFUSED;
        else if (l->npending >= l->backlog)
            full = true;
        else {
            us_link(e);
            e->state = s->state = US_CONNECTED;
            e->peer = s;
            s->peer = e;
            e->name = l->name; /* accepted sockets carry the listener's name */
            e->peer_name = s->name;
            s->peer_name = l->name;
            e->cred = l->cred;
            current_cred(&e->peer_cred);
            s->peer_cred = l->cred;
            if (l->pending_tail)
                l->pending_tail->next_pending = e;
            else
                l->pending_head = e;
            l->pending_tail = e;
            l->npending++;
            e = NULL;
            wake(l);
        }
        spin_unlock_irqrestore(&unix_lock, fl);
        if (node)
            vnode_put(node);
        kfree(e); /* never linked if still ours */
        if (!full)
            return r;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (signal_pending_current())
            return -EINTR;
        thread_sleep_ms(4); /* backlog full: the listener may go away, so poll */
    }
}

static bool has_pending(void *arg) { return __atomic_load_n(&((struct usock *)arg)->pending_head, __ATOMIC_ACQUIRE) != NULL; }

int64_t unix_accept(struct file *f, uint64_t uaddr, uint64_t ulen, uint64_t flags) {
    struct usock *s = f->vnode->data;
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    struct usock *e = NULL;
    for (;;) {
        int64_t r = 0;
        uint64_t fl = spin_lock_irqsave(&unix_lock);
        if (s->state != US_LISTENING)
            r = -EINVAL;
        else if ((e = s->pending_head)) {
            s->pending_head = e->next_pending;
            if (!s->pending_head)
                s->pending_tail = NULL;
            e->next_pending = NULL;
            s->npending--;
        }
        spin_unlock_irqrestore(&unix_lock, fl);
        if (r < 0)
            return r;
        if (e)
            break;
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (waitq_wait_cond(&s->wq, has_pending, s) < 0)
            return -EINTR;
    }
    struct uaddr peer_name = e->peer_name; /* the new descriptor may be closed at once by another thread */
    int fd = us_install(e, flags);
    if (fd >= 0 && uaddr && write_uaddr(uaddr, ulen, &peer_name) < 0)
        return -EFAULT; /* as Linux, the connection stays installed */
    return fd;
}

/* ---- data ---- */

/* Lock held. Appends to `to`'s receive queue and takes ownership of the
 * files. Small stream writes join the previous segment. */
static int enqueue_locked(struct usock *to, const uint8_t *buf, size_t len, struct file **fds, int nfds,
                          const struct usock *from) {
    struct useg *t = to->rx_tail;
    bool stream = to->type == SOCK_STREAM;
    struct linux_ucred cred;
    current_cred(&cred);
    /* stream writes join the last segment only if from the same sender */
    if (stream && !nfds && t && !t->nfds && t->cap - t->len >= len && t->cred.pid == cred.pid) {
        memcpy(t->data + t->len, buf, len);
        t->len += len;
    } else {
        size_t cap = stream && len < 4096 ? 4096 : len;
        struct useg *g = kmalloc(sizeof *g + cap);
        if (!g)
            return -ENOMEM;
        *g = (struct useg){.len = len, .cap = cap, .cred = cred};
        memcpy(g->data, buf, len);
        if (to->type == SOCK_DGRAM && from && from->bound && (g->from = kmalloc(sizeof *g->from)))
            *g->from = from->name;
        if (nfds) {
            g->fds = kmalloc(sizeof *fds * (size_t)nfds);
            if (!g->fds) {
                kfree(g->from);
                kfree(g);
                return -ENOMEM;
            }
            memcpy(g->fds, fds, sizeof *fds * (size_t)nfds);
            g->nfds = nfds;
        }
        if (t)
            t->next = g;
        else
            to->rx_head = g;
        to->rx_tail = g;
    }
    to->rx_bytes += len;
    wake(to);
    return 0;
}

struct send_wait {
    struct usock *s;
    size_t need;
};

static bool can_send(void *arg) {
    struct send_wait *w = arg;
    uint64_t f = spin_lock_irqsave(&unix_lock);
    struct usock *s = w->s;
    bool ok = s->snd_shut || s->state != US_CONNECTED || !s->peer || room(s->peer) >= w->need;
    spin_unlock_irqrestore(&unix_lock, f);
    return ok;
}

/* Lock held: why a connection-oriented send cannot go ahead, or 0. */
static int conn_send_error(struct usock *s) {
    if (s->snd_shut)
        return -EPIPE;
    if (s->state != US_CONNECTED)
        return -ENOTCONN;
    return s->peer ? 0 : -EPIPE;
}

/* Stream send: queues as much as fits, blocking for the rest unless
 * non-blocking. Files ride on the first segment; *nfds becomes 0 once they
 * are queued. */
static int64_t stream_send(struct usock *s, const uint8_t *buf, size_t len, struct file **fds, int *nfds,
                           bool nonblock) {
    size_t done = 0;
    while (done < len) {
        size_t n = 0;
        uint64_t f = spin_lock_irqsave(&unix_lock);
        int err = conn_send_error(s);
        if (!err) {
            n = room(s->peer);
            if (n > len - done)
                n = len - done;
            if (n) {
                err = enqueue_locked(s->peer, buf + done, n, fds, *nfds, s);
                if (!err)
                    *nfds = 0;
            }
        }
        spin_unlock_irqrestore(&unix_lock, f);
        if (err)
            return done ? (int64_t)done : err;
        done += n;
        if (n || done == len)
            continue;
        if (nonblock)
            return done ? (int64_t)done : -EAGAIN;
        struct send_wait w = {s, 1};
        if (waitq_wait_cond(&s->wq, can_send, &w) < 0)
            return done ? (int64_t)done : -EINTR;
    }
    return (int64_t)done;
}

/* Seqpacket and datagram send: the whole message or nothing. */
static int64_t msg_send(struct usock *s, const uint8_t *buf, size_t len, struct file **fds, int *nfds,
                        const struct uaddr *to, bool nonblock) {
    if (len > UNIX_BUF)
        return -EMSGSIZE;
    for (;;) {
        struct vnode *node = NULL;
        if (to && s->type == SOCK_DGRAM) {
            int r = lookup_node(to, &node);
            if (r < 0)
                return r;
        }
        int err = 0;
        bool sent = false;
        uint64_t f = spin_lock_irqsave(&unix_lock);
        struct usock *dst = NULL;
        if (s->type == SOCK_SEQPACKET) {
            err = conn_send_error(s);
            dst = s->peer;
        } else if (to) {
            dst = resolve_locked(to, node);
            err = dst ? 0 : -ECONNREFUSED;
        } else if (s->dgram_connected) {
            dst = s->peer;
            err = dst ? 0 : -ECONNREFUSED;
        } else {
            err = -ENOTCONN;
        }
        if (!err && dst->type != s->type)
            err = -EPROTOTYPE;
        if (!err && dst->rcv_shut)
            err = s->type == SOCK_DGRAM ? -ECONNREFUSED : -EPIPE;
        if (!err && room(dst) >= len && (len || dst->rx_bytes < UNIX_BUF)) {
            err = enqueue_locked(dst, buf, len, fds, *nfds, s);
            if (!err) {
                *nfds = 0;
                sent = true;
            }
        }
        spin_unlock_irqrestore(&unix_lock, f);
        if (node)
            vnode_put(node);
        if (err)
            return err;
        if (sent)
            return (int64_t)len;
        if (nonblock)
            return -EAGAIN;
        if (s->type == SOCK_SEQPACKET) {
            struct send_wait w = {s, len};
            if (waitq_wait_cond(&s->wq, can_send, &w) < 0)
                return -EINTR;
        } else {
            /* A datagram receiver does not know its senders, so it cannot
             * wake them: poll for room. */
            if (signal_pending_current())
                return -EINTR;
            thread_sleep_ms(4);
        }
    }
}

struct urecv {
    struct file **fds; /* received files, owned until installed */
    int nfds;
    struct uaddr from;
    bool has_from;
    struct linux_ucred cred; /* the sender of what was read */
    bool has_cred;
    int msg_flags;
};

static void take_fds(struct useg *g, struct urecv *out) {
    out->fds = g->fds;
    out->nfds = g->nfds;
    g->fds = NULL;
    g->nfds = 0;
}

/* Lock held. Removes the (fully read) head segment. */
static void pop_head(struct usock *s) {
    struct useg *g = s->rx_head;
    s->rx_head = g->next;
    if (!s->rx_head)
        s->rx_tail = NULL;
    kfree(g->fds);
    kfree(g->from);
    kfree(g);
}

/* Lock held. Stream read across segments, stopping at file boundaries. */
static size_t take_stream_locked(struct usock *s, uint8_t *buf, size_t len, bool peek, struct urecv *out) {
    size_t got = 0;
    struct useg *g = s->rx_head;
    size_t off = g ? g->off : 0;
    while (g && got < len) {
        if (g->nfds && (got || out->nfds))
            break; /* its files go out with its own first byte, in the next read */
        if (!out->has_cred)
            out->cred = g->cred, out->has_cred = true;
        size_t n = g->len - off < len - got ? g->len - off : len - got;
        memcpy(buf + got, g->data + off, n);
        got += n;
        off += n;
        bool files = g->nfds > 0, finished = off == g->len;
        struct useg *next = g->next;
        if (!peek) {
            if (files)
                take_fds(g, out);
            g->off = off;
            s->rx_bytes -= n;
            if (finished)
                pop_head(s);
        }
        if (files || !finished)
            break;
        g = next;
        off = g ? g->off : 0;
    }
    return got;
}

/* Lock held. One message (seqpacket, datagram); the rest of a message
 * longer than the buffer is discarded, as on Linux. */
static int64_t take_msg_locked(struct usock *s, uint8_t *buf, size_t len, int flags, struct urecv *out) {
    struct useg *g = s->rx_head;
    size_t n = g->len < len ? g->len : len;
    memcpy(buf, g->data, n);
    if (g->len > len)
        out->msg_flags |= MSG_TRUNC;
    out->cred = g->cred, out->has_cred = true;
    if (g->from) {
        out->from = *g->from;
        out->has_from = true;
    }
    int64_t ret = (flags & MSG_TRUNC) ? (int64_t)g->len : (int64_t)n;
    if (!(flags & MSG_PEEK)) {
        take_fds(g, out);
        s->rx_bytes -= g->len;
        pop_head(s);
    }
    return ret;
}

static bool can_recv(void *arg) {
    struct usock *s = arg;
    uint64_t f = spin_lock_irqsave(&unix_lock);
    bool ok = s->rx_head || s->rcv_shut || s->state == US_LISTENING ||
              (s->type != SOCK_DGRAM && (s->state != US_CONNECTED || !s->peer));
    spin_unlock_irqrestore(&unix_lock, f);
    return ok;
}

static int64_t us_recv(struct usock *s, uint8_t *buf, size_t len, int flags, bool nonblock, struct urecv *out) {
    bool peek = flags & MSG_PEEK;
    size_t got = 0;
    if (!len && s->type == SOCK_STREAM)
        return 0;
    for (;;) {
        int64_t r = -EAGAIN;
        uint64_t f = spin_lock_irqsave(&unix_lock);
        if (s->state == US_LISTENING || (s->type == SOCK_STREAM && s->state != US_CONNECTED))
            r = -EINVAL;
        else if (s->type == SOCK_SEQPACKET && s->state != US_CONNECTED)
            r = -ENOTCONN;
        else if (s->rx_head && s->type == SOCK_STREAM)
            r = (int64_t)(got += take_stream_locked(s, buf + got, len - got, peek, out));
        else if (s->rx_head)
            r = take_msg_locked(s, buf, len, flags, out);
        else if (s->rcv_shut || (s->type != SOCK_DGRAM && !s->peer))
            r = (int64_t)got; /* end of stream */
        if (r >= 0 && !peek) {
            wake(s); /* room for writers: they wait on their own queue */
            wake(s->peer);
        }
        bool more = s->type == SOCK_STREAM && (flags & MSG_WAITALL) && !peek && r >= 0 && got < len &&
                    !out->nfds && !s->rcv_shut && s->peer;
        spin_unlock_irqrestore(&unix_lock, f);
        if (r >= 0 && !more)
            return r;
        if (r < 0 && r != -EAGAIN)
            return got ? (int64_t)got : r;
        if (nonblock)
            return got ? (int64_t)got : -EAGAIN;
        if (waitq_wait_cond(&s->wq, can_recv, s) < 0)
            return got ? (int64_t)got : -EINTR;
    }
}

static void drop_files(struct file **fds, int n) {
    for (int i = 0; i < n; i++)
        file_put(fds[i]);
}

/* ---- control messages ---- */

/* Collects the files named by SCM_RIGHTS messages (referenced). Credentials
 * are accepted and ignored. */
static int parse_control(uint64_t ucontrol, uint64_t len, struct file **files, int *nfiles) {
    *nfiles = 0;
    if (!ucontrol || len < sizeof(struct linux_cmsghdr))
        return 0;
    if (len > UNIX_MAX_CONTROL)
        return -ENOBUFS;
    uint8_t *cb = kmalloc(len);
    if (!cb)
        return -ENOMEM;
    int r = copy_from_user(cb, ucontrol, len) < 0 ? -EFAULT : 0;
    for (uint64_t off = 0; r == 0 && off + sizeof(struct linux_cmsghdr) <= len;) {
        struct linux_cmsghdr h;
        memcpy(&h, cb + off, sizeof h);
        if (h.len < sizeof h || h.len > len - off) {
            r = -EINVAL;
            break;
        }
        if (h.level == SOL_SOCKET && h.type == SCM_RIGHTS) {
            int n = (int)((h.len - sizeof h) / sizeof(int32_t));
            if (*nfiles + n > UNIX_MAX_FDS) {
                r = -EINVAL;
                break;
            }
            for (int i = 0; i < n; i++) {
                int32_t fd;
                memcpy(&fd, cb + off + sizeof h + (size_t)i * sizeof fd, sizeof fd);
                struct file *file = fd_get(fd);
                if (!file) {
                    r = -EBADF;
                    break;
                }
                files[(*nfiles)++] = file;
            }
        } else if (h.level != SOL_SOCKET || h.type != SCM_CREDENTIALS) {
            r = -EINVAL;
        }
        off += ALIGN_UP(h.len, 8);
    }
    kfree(cb);
    if (r < 0) {
        drop_files(files, *nfiles);
        *nfiles = 0;
    }
    return r;
}

/* Installs received files in the caller and writes the SCM_RIGHTS message.
 * Files that do not fit are closed and MSG_CTRUNC is set. Returns the
 * control bytes used. */
static uint64_t deliver_files(struct urecv *in, uint64_t ucontrol, uint64_t controllen, bool cloexec) {
    size_t hdr = sizeof(struct linux_cmsghdr);
    int fit = controllen >= hdr + sizeof(int32_t) && ucontrol ? (int)((controllen - hdr) / sizeof(int32_t)) : 0;
    if (fit > in->nfds)
        fit = in->nfds;
    int32_t *fds = kmalloc(sizeof(int32_t) * (size_t)(fit ? fit : 1));
    int installed = 0;
    while (fds && installed < fit) {
        int fd = fd_install(in->fds[installed], 0, cloexec);
        if (fd < 0)
            break;
        fds[installed++] = fd;
    }
    drop_files(in->fds + installed, in->nfds - installed);
    if (installed < in->nfds)
        in->msg_flags |= MSG_CTRUNC;
    uint64_t used = 0;
    if (installed) {
        struct linux_cmsghdr h = {hdr + sizeof(int32_t) * (size_t)installed, SOL_SOCKET, SCM_RIGHTS};
        if (copy_to_user(ucontrol, &h, hdr) < 0 ||
            copy_to_user(ucontrol + hdr, fds, sizeof(int32_t) * (size_t)installed) < 0) {
            for (int i = 0; i < installed; i++)
                fd_close(fds[i]);
            in->msg_flags |= MSG_CTRUNC;
        } else {
            used = ALIGN_UP(h.len, 8);
            if (used > controllen)
                used = controllen;
        }
    }
    kfree(fds);
    kfree(in->fds);
    in->fds = NULL;
    in->nfds = 0;
    return used;
}

/* ---- iovecs ---- */

static int64_t iov_total(const struct linux_iovec *iov, uint64_t cnt) {
    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        if (iov[i].iov_len > 0x7fffffffULL || total + iov[i].iov_len > 0x7fffffffULL)
            return -EINVAL;
        total += iov[i].iov_len;
    }
    return (int64_t)total;
}

/* Copies `len` bytes at logical offset `skip` of the iovecs from or to a
 * kernel buffer. */
static int iov_copy(const struct linux_iovec *iov, uint64_t cnt, size_t skip, void *kbuf, size_t len, bool to_user) {
    uint8_t *k = kbuf;
    for (uint64_t i = 0; i < cnt && len; i++) {
        if (skip >= iov[i].iov_len) {
            skip -= iov[i].iov_len;
            continue;
        }
        size_t n = iov[i].iov_len - skip < len ? iov[i].iov_len - skip : len;
        int r = to_user ? copy_to_user(iov[i].iov_base + skip, k, n) : copy_from_user(k, iov[i].iov_base + skip, n);
        if (r < 0)
            return -EFAULT;
        k += n;
        len -= n;
        skip = 0;
    }
    return 0;
}

int64_t unix_sendmsg(struct file *f, const struct linux_iovec *iov, uint64_t iovcnt, uint64_t uname, uint32_t namelen,
                     uint64_t ucontrol, uint64_t controllen, uint64_t flags) {
    struct usock *s = f->vnode->data;
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    int64_t total = iov_total(iov, iovcnt);
    if (total < 0)
        return total;
    struct uaddr to;
    bool has_to = false;
    if (uname && namelen) {
        if (s->type != SOCK_DGRAM)
            return s->state == US_CONNECTED ? -EISCONN : -EOPNOTSUPP;
        int r = read_uaddr(uname, namelen, &to);
        if (r < 0)
            return r;
        has_to = true;
    }
    struct file **files = NULL;
    if (ucontrol && controllen >= sizeof(struct linux_cmsghdr) && !(files = kmalloc(sizeof *files * UNIX_MAX_FDS)))
        return -ENOMEM;
    int nfiles;
    int64_t r = parse_control(ucontrol, controllen, files, &nfiles);
    if (r == 0 && s->type == SOCK_STREAM) {
        size_t chunk = total < UNIX_CHUNK ? (size_t)total : UNIX_CHUNK;
        uint8_t *kb = kmalloc(chunk ? chunk : 1);
        size_t done = 0;
        r = kb ? 0 : -ENOMEM;
        while (r == 0 && done < (size_t)total) {
            size_t n = (size_t)total - done < chunk ? (size_t)total - done : chunk;
            if (iov_copy(iov, iovcnt, done, kb, n, false) < 0) {
                r = -EFAULT;
                break;
            }
            int64_t sent = stream_send(s, kb, n, files, &nfiles, nonblock);
            if (sent < 0) {
                r = sent;
                break;
            }
            done += (size_t)sent;
            if ((size_t)sent < n)
                break;
        }
        if (done)
            r = (int64_t)done;
        kfree(kb);
    } else if (r == 0) {
        uint8_t *kb = total <= UNIX_BUF ? kmalloc(total ? (size_t)total : 1) : NULL;
        if (total > UNIX_BUF)
            r = -EMSGSIZE;
        else if (!kb)
            r = -ENOMEM;
        else if (iov_copy(iov, iovcnt, 0, kb, (size_t)total, false) < 0)
            r = -EFAULT;
        else
            r = msg_send(s, kb, (size_t)total, files, &nfiles, has_to ? &to : NULL, nonblock);
        kfree(kb);
    }
    drop_files(files, nfiles); /* not sent */
    kfree(files);
    if (r == -EPIPE && !(flags & MSG_NOSIGNAL))
        proc_kill(proc_current(), SIGPIPE);
    return r;
}

int64_t unix_recvmsg(struct file *f, const struct linux_iovec *iov, uint64_t iovcnt, uint64_t uname, uint32_t *namelen,
                     uint64_t ucontrol, uint64_t *controllen, int *msg_flags, uint64_t flags) {
    struct usock *s = f->vnode->data;
    bool nonblock = (f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT);
    int64_t total = iov_total(iov, iovcnt);
    if (total < 0)
        return total;
    size_t len = total < UNIX_BUF ? (size_t)total : UNIX_BUF;
    uint8_t *kb = kmalloc(len ? len : 1);
    if (!kb)
        return -ENOMEM;
    struct urecv in = {0};
    int64_t r = us_recv(s, kb, len, (int)flags, nonblock, &in);
    if (r > 0 && iov_copy(iov, iovcnt, 0, kb, (size_t)r < len ? (size_t)r : len, true) < 0)
        r = -EFAULT;
    kfree(kb);
    uint64_t used = 0;
    if (in.nfds) {
        if (r >= 0)
            used = deliver_files(&in, ucontrol, *controllen, flags & MSG_CMSG_CLOEXEC);
        else {
            drop_files(in.fds, in.nfds);
            kfree(in.fds);
        }
    }
    if (r >= 0 && s->passcred && in.has_cred && ucontrol) {
        /* SO_PASSCRED: who sent it, as the receiver's namespaces name them */
        size_t at = (used + 7) & ~(size_t)7, hdr = sizeof(struct linux_cmsghdr);
        struct linux_ucred c = {pid_to_user(in.cred.pid), uid_to_user(in.cred.uid), gid_to_user(in.cred.gid)};
        struct linux_cmsghdr h = {hdr + sizeof c, SOL_SOCKET, SCM_CREDENTIALS};
        if (at + hdr + sizeof c <= *controllen) {
            if (copy_to_user(ucontrol + at, &h, hdr) < 0 || copy_to_user(ucontrol + at + hdr, &c, sizeof c) < 0)
                r = -EFAULT;
            used = at + ((hdr + sizeof c + 7) & ~(size_t)7);
            if (used > *controllen)
                used = *controllen;
        } else {
            in.msg_flags |= MSG_CTRUNC;
        }
    }
    if (r >= 0) {
        *controllen = used;
        *msg_flags = in.msg_flags;
        if (uname && in.has_from) {
            uint32_t full = 2 + in.from.len;
            uint8_t buf[2 + UNIX_PATH_MAX + 1] = {AF_UNIX, 0};
            memcpy(buf + 2, in.from.path, in.from.len);
            if (copy_to_user(uname, buf, *namelen < full ? *namelen : full) < 0)
                r = -EFAULT;
            *namelen = full;
        } else {
            *namelen = 0;
        }
    }
    return r;
}

/* ---- file operations ---- */

static int64_t uf_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct urecv in = {0};
    int64_t r = us_recv(v->data, buf, len, 0, f && (f->flags & O_NONBLOCK), &in);
    drop_files(in.fds, in.nfds); /* read() has nowhere to put them */
    kfree(in.fds);
    return r;
}

static int64_t uf_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)off;
    struct usock *s = v->data;
    bool nonblock = f && (f->flags & O_NONBLOCK);
    int nfds = 0;
    int64_t r = s->type == SOCK_STREAM ? stream_send(s, buf, len, NULL, &nfds, nonblock)
                                       : msg_send(s, buf, len, NULL, &nfds, NULL, nonblock);
    if (r == -EPIPE)
        proc_kill(proc_current(), SIGPIPE);
    return r;
}

static int uf_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct usock *s = v->data;
    int ev = 0;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    if (s->state == US_LISTENING) {
        if (s->pending_head)
            ev |= POLLIN;
    } else {
        if (s->rx_head)
            ev |= POLLIN;
        if (s->rcv_shut)
            ev |= POLLIN | POLLRDHUP;
        if (s->rcv_shut && s->snd_shut)
            ev |= POLLHUP;
        if (s->type != SOCK_DGRAM && s->state == US_UNCONNECTED)
            ev |= POLLHUP | POLLOUT; /* Linux: an unconnected stream socket is "closed" */
        /* Writable once a quarter of the peer's queue is free, so a poller
         * is not woken for every byte drained (Linux uses a similar rule). */
        struct usock *dst = s->peer;
        if (s->state == US_CONNECTED || s->type == SOCK_DGRAM)
            if (s->snd_shut || !dst || room(dst) >= UNIX_BUF / 4)
                ev |= POLLOUT;
    }
    spin_unlock_irqrestore(&unix_lock, fl);
    if (wq)
        *wq = &s->wq;
    return ev;
}

static int64_t uf_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)f;
    struct usock *s = v->data;
    if (req >= 0x8910 && req <= 0x89ff) /* SIOCGIF*: interfaces, asked of any socket (if_nametoindex) */
        return net_if_ioctl(req, arg);
    if (req != FIONREAD)
        return -ENOTTY;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    int n = s->type == SOCK_STREAM ? (int)s->rx_bytes : s->rx_head ? (int)s->rx_head->len : 0;
    spin_unlock_irqrestore(&unix_lock, fl);
    return copy_to_user(arg, &n, sizeof n);
}

static const struct vnode_ops unix_ops = {
    .read = uf_read, .write = uf_write, .poll = uf_poll, .ioctl = uf_ioctl, .release = us_release,
};

/* ---- the rest of the socket calls ---- */

static int check_type(uint64_t type, uint64_t protocol) {
    int t = (int)(type & 0xf);
    if (type & ~(uint64_t)(0xf | SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET)
        return -ESOCKTNOSUPPORT;
    if (protocol && protocol != AF_UNIX)
        return -EPROTONOSUPPORT;
    return t;
}

int64_t unix_socket(uint64_t type, uint64_t protocol) {
    int t = check_type(type, protocol);
    if (t < 0)
        return t;
    struct usock *s = us_new(t);
    if (!s)
        return -ENOMEM;
    uint64_t f = spin_lock_irqsave(&unix_lock);
    us_link(s);
    spin_unlock_irqrestore(&unix_lock, f);
    return us_install(s, type);
}

int64_t unix_socketpair(uint64_t type, uint64_t protocol, uint64_t usv) {
    int t = check_type(type, protocol);
    if (t < 0)
        return t;
    struct usock *a = us_new(t), *b = us_new(t);
    if (!a || !b) {
        kfree(a);
        kfree(b);
        return -ENOMEM;
    }
    uint64_t f = spin_lock_irqsave(&unix_lock);
    us_link(a);
    us_link(b);
    a->peer = b;
    b->peer = a;
    if (t == SOCK_DGRAM)
        a->dgram_connected = b->dgram_connected = true;
    else
        a->state = b->state = US_CONNECTED;
    a->peer_cred = b->cred;
    b->peer_cred = a->cred;
    spin_unlock_irqrestore(&unix_lock, f);
    int fds[2];
    fds[0] = us_install(a, type);
    if (fds[0] < 0) {
        f = spin_lock_irqsave(&unix_lock); /* `a` is gone; take `b` down too */
        struct reap r = {NULL, NULL};
        us_destroy_locked(b, &r);
        spin_unlock_irqrestore(&unix_lock, f);
        reap_finish(&r);
        return fds[0];
    }
    fds[1] = us_install(b, type);
    if (fds[1] < 0) {
        fd_close(fds[0]);
        return fds[1];
    }
    if (copy_to_user(usv, fds, sizeof fds) < 0) {
        fd_close(fds[0]);
        fd_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}

int64_t unix_shutdown(struct file *f, uint64_t how) {
    struct usock *s = f->vnode->data;
    if (how > SHUT_RDWR)
        return -EINVAL;
    int64_t r = 0;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    if (s->type != SOCK_DGRAM && s->state != US_CONNECTED) {
        r = -ENOTCONN;
    } else {
        bool rd = how != SHUT_WR, wr = how != SHUT_RD;
        s->rcv_shut |= rd;
        s->snd_shut |= wr;
        struct usock *p = s->type != SOCK_DGRAM ? s->peer : NULL;
        if (p) { /* the other end sees the mirror image */
            p->snd_shut |= rd;
            p->rcv_shut |= wr;
            wake(p);
        }
        wake(s);
    }
    spin_unlock_irqrestore(&unix_lock, fl);
    return r;
}

int64_t unix_getname(struct file *f, uint64_t uaddr, uint64_t ulen, bool peer) {
    struct usock *s = f->vnode->data;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    bool connected = s->state == US_CONNECTED || s->dgram_connected;
    struct uaddr a = peer ? s->peer_name : s->name;
    spin_unlock_irqrestore(&unix_lock, fl);
    if (peer && !connected)
        return -ENOTCONN;
    return write_uaddr(uaddr, ulen, &a);
}

int64_t unix_getsockopt(struct file *f, uint64_t level, uint64_t opt, uint64_t uval, uint64_t ulen) {
    struct usock *s = f->vnode->data;
    if (level != SOL_SOCKET)
        return -ENOPROTOOPT;
    int value = 0;
    struct linux_ucred cred;
    const void *src = &value;
    uint32_t size = sizeof value;
    uint64_t fl = spin_lock_irqsave(&unix_lock);
    switch (opt) {
    case SO_TYPE:
        value = s->type;
        break;
    case SO_ERROR:
        break;
    case SO_SNDBUF:
    case SO_RCVBUF:
        value = UNIX_BUF;
        break;
    case SO_PASSCRED:
        value = s->passcred;
        break;
    case SO_PEERCRED: /* named as the caller's namespaces name them */
        cred = s->peer_cred;
        cred.pid = pid_to_user(cred.pid);
        if (cred.uid != (uint32_t)-1)
            cred.uid = uid_to_user(cred.uid);
        if (cred.gid != (uint32_t)-1)
            cred.gid = gid_to_user(cred.gid);
        src = &cred;
        size = sizeof cred;
        break;
    case SO_ACCEPTCONN:
        value = s->state == US_LISTENING;
        break;
    case SO_PROTOCOL:
        break;
    case SO_DOMAIN:
        value = AF_UNIX;
        break;
    default:
        size = 0;
    }
    spin_unlock_irqrestore(&unix_lock, fl);
    if (!size)
        return -ENOPROTOOPT;
    uint32_t len;
    if (copy_from_user(&len, ulen, sizeof len) < 0)
        return -EFAULT;
    if ((int32_t)len < 0)
        return -EINVAL;
    if (len > size)
        len = size;
    if (copy_to_user(uval, src, len) < 0 || copy_to_user(ulen, &len, sizeof len) < 0)
        return -EFAULT;
    return 0;
}

int64_t unix_setsockopt(struct file *f, uint64_t level, uint64_t opt, uint64_t uval, uint64_t len) {
    struct usock *s = f->vnode->data;
    if (level == SOL_SOCKET && opt == SO_PASSCRED) {
        int on;
        if (len < sizeof on || copy_from_user(&on, uval, sizeof on) < 0)
            return -EFAULT;
        s->passcred = on != 0; /* every message received then says who sent it */
    }
    return 0; /* buffer sizes and the rest: accepted, defaults kept */
}
