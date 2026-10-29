#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>
#include "sock.h"

/* Linux socket API for AF_INET stream and datagram sockets. A socket is a
 * file (vnode type VN_SOCK), so read/write/poll/close work on it too.
 * AF_UNIX sockets live in unix.c; the system calls below hand them over. */

struct sock *all_socks;
static uint16_t next_port = 49152;

struct sock *sock_alloc(int type) {
    struct sock *s = kzalloc(sizeof *s);
    if (!s)
        return NULL;
    s->type = type;
    waitq_init(&s->wq);
    if (type == 1) {
        s->rx = kmalloc(SOCK_RX_SIZE);
        s->tx = kmalloc(SOCK_TX_SIZE);
        if (!s->rx || !s->tx) {
            kfree(s->rx);
            kfree(s->tx);
            kfree(s);
            return NULL;
        }
    }
    s->next = all_socks;
    all_socks = s;
    return s;
}

/* Frees a socket once no file refers to it and TCP is finished with it. */
void sock_free_if_done(struct sock *s) {
    if (s->file_open || (s->type == 1 && s->state != TCP_CLOSED))
        return;
    for (struct sock **pp = &all_socks; *pp; pp = &(*pp)->next)
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    for (int i = 0; i < s->q_count; i++)
        kfree(s->q[(s->q_head + i) % UDP_QUEUE].data);
    kfree(s->rx);
    kfree(s->tx);
    kfree(s);
}

uint16_t sock_ephemeral_port(void) {
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port++;
        if (next_port < 49152)
            next_port = 49152;
        bool used = false;
        for (struct sock *s = all_socks; s; s = s->next)
            if (s->lport == p)
                used = true;
        if (!used)
            return p;
    }
    return 0;
}

/* The socket a packet from rip:rport to lip:lport belongs to: a connection
 * first, else a listener (TCP) or an unconnected socket (UDP). A socket
 * bound to one address only sees packets sent to it (127.0.0.1 is not the
 * NIC's address); one bound to a specific address beats a wildcard one. */
struct sock *sock_find(int type, uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport) {
    struct sock *listener = NULL, *loose = NULL;
    for (struct sock *s = all_socks; s; s = s->next) {
        if (s->type != type || s->lport != lport || (s->lip && s->lip != lip))
            continue;
        if (type == 1) {
            if (s->state == TCP_LISTEN) {
                if (!listener || s->lip)
                    listener = s;
            } else if (s->rip == rip && s->rport == rport && s->state != TCP_CLOSED) {
                return s;
            }
        } else {
            if (s->rip == rip && s->rport == rport)
                return s;
            if (!s->rip && (!loose || s->lip))
                loose = s;
        }
    }
    return type == 1 ? listener : loose;
}

/* True when the advertised receive window has (nearly) closed, so reading
 * must tell the peer it may send again. */
static bool rx_window_closed(struct sock *s) { return SOCK_RX_SIZE - (s->rx_head - s->rx_tail) < 4096; }

/* ---- file operations ---- */

struct sock_wait {
    struct sock *s;
    int what; /* 0 readable, 1 writable, 2 connected, 3 acceptable */
};

static bool sock_ready(void *arg) {
    struct sock_wait *w = arg;
    struct sock *s = w->s;
    if (s->error)
        return true;
    switch (w->what) {
    case 0:
        return s->type == 1 ? (s->rx_head != s->rx_tail || s->rx_eof || s->state == TCP_CLOSED) : s->q_count > 0;
    case 1:
        return s->tx_len < SOCK_TX_SIZE || s->state == TCP_CLOSED;
    case 2:
        return s->state != TCP_SYN_SENT;
    case 3:
        return s->accept_count > 0;
    }
    return true;
}

static int sock_wait(struct sock *s, int what, bool nonblock) {
    struct sock_wait w = {s, what};
    uint64_t f = spin_lock_irqsave(&net_lock);
    bool ready = sock_ready(&w);
    net_unlock(f);
    if (ready)
        return 0;
    if (nonblock)
        return -EAGAIN;
    return waitq_wait_cond(&s->wq, sock_ready, &w);
}

static int64_t sock_recv(struct sock *s, void *buf, size_t len, bool nonblock, uint32_t *from, uint16_t *fport,
                         bool peek) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        if (s->type == 1) {
            uint64_t avail = s->rx_head - s->rx_tail;
            if (avail) {
                size_t n = avail < len ? avail : len;
                size_t at = s->rx_tail % SOCK_RX_SIZE;
                size_t first = n < SOCK_RX_SIZE - at ? n : SOCK_RX_SIZE - at;
                memcpy(buf, s->rx + at, first);
                memcpy((uint8_t *)buf + first, s->rx, n - first);
                bool was_full = rx_window_closed(s);
                if (!peek)
                    s->rx_tail += n;
                if (was_full && !peek && s->state >= TCP_ESTABLISHED)
                    tcp_send_ack(s); /* window reopened */
                net_unlock(f);
                return (int64_t)n;
            }
            int err = s->error;
            bool eof = s->rx_eof || s->state == TCP_CLOSED || s->shut_rd;
            net_unlock(f);
            if (err)
                return err == ECONNRESET && eof ? 0 : -err;
            if (eof)
                return 0;
        } else {
            if (s->q_count) {
                struct dgram *d = &s->q[s->q_head];
                size_t n = d->len < len ? d->len : len;
                memcpy(buf, d->data, n);
                if (from)
                    *from = d->src;
                if (fport)
                    *fport = d->sport;
                if (!peek) {
                    kfree(d->data);
                    s->q_head = (s->q_head + 1) % UDP_QUEUE;
                    s->q_count--;
                }
                net_unlock(f);
                return (int64_t)n;
            }
            net_unlock(f);
        }
        int r = sock_wait(s, 0, nonblock);
        if (r < 0)
            return r;
    }
}

static int64_t sock_send(struct sock *s, const void *buf, size_t len, bool nonblock, uint32_t dst, uint16_t dport) {
    if (!netns_is_initial(netns_current()))
        return -ENETUNREACH;
    if (s->type == 2) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        if (!dst) {
            dst = s->rip;
            dport = s->rport;
        }
        int r = dst ? udp_sendto(s, dst, dport, buf, len) : -EDESTADDRREQ;
        net_unlock(f);
        return r;
    }
    size_t done = 0;
    while (done < len) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        int64_t n = tcp_send(s, (const uint8_t *)buf + done, len - done);
        net_unlock(f);
        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n != -EAGAIN)
            return done ? (int64_t)done : n;
        if (nonblock)
            return done ? (int64_t)done : -EAGAIN;
        int r = sock_wait(s, s->state == TCP_SYN_SENT ? 2 : 1, false);
        if (r < 0)
            return done ? (int64_t)done : r;
    }
    return (int64_t)done;
}

static int64_t sf_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    return sock_recv(v->data, buf, len, f && (f->flags & O_NONBLOCK), NULL, NULL, false);
}

static int64_t sf_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)off;
    int64_t r = sock_send(v->data, buf, len, f && (f->flags & O_NONBLOCK), 0, 0);
    if (r == -EPIPE)
        proc_kill(proc_current(), SIGPIPE);
    return r;
}

static int sf_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct sock *s = v->data;
    uint64_t fl = spin_lock_irqsave(&net_lock);
    int ev = 0;
    struct sock_wait r = {s, s->state == TCP_LISTEN ? 3 : 0}, w = {s, 1};
    if (sock_ready(&r))
        ev |= POLLIN;
    if (s->type == 2 || (s->state != TCP_SYN_SENT && s->state != TCP_LISTEN && sock_ready(&w)))
        ev |= POLLOUT;
    if (s->error)
        ev |= POLLERR;
    /* As Linux: the peer's FIN makes the socket readable (EOF) and sets
     * POLLRDHUP; POLLHUP means both directions are done. A half-closed
     * connection is not hung up: an HTTP client may shutdown(SHUT_WR) after
     * its request and still wait for the answer. */
    if (s->type == 1 && s->rx_eof)
        ev |= POLLIN | POLLRDHUP;
    if (s->type == 1 && s->state != TCP_LISTEN && (s->state == TCP_CLOSED || (s->rx_eof && s->fin_queued)))
        ev |= POLLHUP | POLLIN;
    net_unlock(fl);
    if (wq)
        *wq = &s->wq;
    return ev;
}

static void sf_release(struct vnode *v) {
    struct sock *s = v->data;
    uint64_t f = spin_lock_irqsave(&net_lock);
    s->file_open = false;
    if (s->type == 1) {
        if (s->state == TCP_LISTEN) {
            for (int i = 0; i < s->accept_count; i++) {
                struct sock *c = s->accept_q[i];
                c->file_open = false;
                c->listener = NULL;
                tcp_close(c);
                sock_free_if_done(c); /* one the peer already reset */
            }
            s->accept_count = 0;
            tcp_listener_closed(s); /* half-open children must not point at us */
        }
        tcp_close(s);
    }
    sock_free_if_done(s);
    net_unlock(f);
}

/* Sockets answer the interface questions (ifconfig, the desktop's tray). */
static int64_t sf_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v, (void)f;
    return net_if_ioctl(req, arg);
}

static const struct vnode_ops sock_ops = {
    .read = sf_read, .write = sf_write, .poll = sf_poll, .release = sf_release, .ioctl = sf_ioctl,
};

static int sock_file(struct sock *s, uint32_t flags, bool cloexec) {
    struct vnode *v = vnode_alloc(VN_SOCK, 0777, &sock_ops);
    if (!v)
        return -ENOMEM;
    v->data = s;
    s->file_open = true;
    struct file *f = file_from_vnode(v, O_RDWR | (flags & O_NONBLOCK));
    vnode_put(v);
    if (!f)
        return -ENOMEM;
    int fd = fd_install(f, 0, cloexec);
    if (fd < 0)
        file_put(f);
    return fd;
}

static struct sock *fd_sock(int fd, struct file **out) {
    struct file *f = fd_get(fd);
    if (!f)
        return NULL;
    if (f->vnode->ops != &sock_ops) {
        file_put(f);
        return NULL;
    }
    *out = f;
    return f->vnode->data;
}

static int read_addr(uint64_t uaddr, uint64_t len, uint32_t *ip, uint16_t *port) {
    struct linux_sockaddr_in a;
    if (len < 8 || copy_from_user(&a, uaddr, len < sizeof a ? len : sizeof a) < 0)
        return -EFAULT;
    if (a.family != AF_INET)
        return -EAFNOSUPPORT;
    *ip = ntohl(a.addr);
    *port = ntohs(a.port);
    return 0;
}

static int write_addr(uint64_t uaddr, uint64_t ulen, uint32_t ip, uint16_t port) {
    if (!uaddr)
        return 0;
    uint32_t len;
    if (copy_from_user(&len, ulen, sizeof len) < 0)
        return -EFAULT;
    struct linux_sockaddr_in a = {.family = AF_INET, .port = htons(port), .addr = htonl(ip)};
    uint32_t n = len < sizeof a ? len : sizeof a;
    if (copy_to_user(uaddr, &a, n) < 0)
        return -EFAULT;
    len = sizeof a;
    return copy_to_user(ulen, &len, sizeof len);
}

/* ---- system calls ---- */

/* AF_NETLINK descriptors are handled by netlink.c. */
static struct file *nl_fd(uint64_t fd) {
    struct file *f = fd_get((int)fd);
    if (f && netlink_is_socket(f))
        return f;
    if (f)
        file_put(f);
    return NULL;
}

#define NL_CALL(fd, expr)                                                                                \
    do {                                                                                                 \
        struct file *nf_ = nl_fd(fd);                                                                    \
        if (nf_) {                                                                                       \
            int64_t r_ = (expr);                                                                         \
            file_put(nf_);                                                                               \
            return r_;                                                                                   \
        }                                                                                                \
    } while (0)

static int64_t nl_sendto(struct file *f, uint64_t ubuf, uint64_t len) {
    if (len > 65536)
        return -EMSGSIZE;
    uint8_t *kb = kmalloc(len ? len : 1);
    if (!kb)
        return -ENOMEM;
    int64_t r = copy_from_user(kb, ubuf, len) < 0 ? -EFAULT : netlink_send(f, kb, len);
    kfree(kb);
    return r;
}

static int64_t nl_msg_io(struct file *f, uint64_t umsg, uint64_t flags, bool send) {
    struct linux_msghdr m;
    if (copy_from_user(&m, umsg, sizeof m) < 0)
        return -EFAULT;
    if (m.iovlen > 64)
        return -EMSGSIZE;
    struct linux_iovec iov[64];
    if (m.iovlen && copy_from_user(iov, m.iov, m.iovlen * sizeof iov[0]) < 0)
        return -EFAULT;
    if (send) { /* the pieces, gathered into one request */
        uint64_t total = 0;
        for (uint64_t i = 0; i < m.iovlen; i++)
            total += iov[i].iov_len;
        if (total > 65536)
            return -EMSGSIZE;
        uint8_t *kb = kmalloc(total ? total : 1);
        if (!kb)
            return -ENOMEM;
        uint64_t off = 0;
        int64_t r = 0;
        for (uint64_t i = 0; i < m.iovlen && r == 0; i++) {
            if (copy_from_user(kb + off, iov[i].iov_base, iov[i].iov_len) < 0)
                r = -EFAULT;
            off += iov[i].iov_len;
        }
        if (r == 0)
            r = netlink_send(f, kb, total);
        kfree(kb);
        return r;
    }
    int msg_flags = 0;
    uint64_t namelen_at = umsg + offsetof(struct linux_msghdr, namelen);
    int64_t r = m.iovlen ? netlink_recv(f, iov[0].iov_base, iov[0].iov_len, flags, m.name, m.name ? namelen_at : 0,
                                        &msg_flags)
                         : 0;
    if (r >= 0) {
        uint64_t zero = 0;
        copy_to_user(umsg + offsetof(struct linux_msghdr, controllen), &zero, sizeof zero);
        copy_to_user(umsg + offsetof(struct linux_msghdr, flags), &msg_flags, sizeof msg_flags);
    }
    return r;
}

static int64_t nl_getsockopt(uint64_t level, uint64_t opt, uint64_t uval, uint64_t ulen) {
    int value = 0;
    if (level == SOL_SOCKET) {
        switch (opt) {
        case SO_TYPE: value = 3; break; /* SOCK_RAW */
        case SO_SNDBUF:
        case SO_RCVBUF: value = 212992; break;
        case SO_DOMAIN: value = AF_NETLINK; break;
        }
    }
    uint32_t len = sizeof value;
    if (copy_to_user(uval, &value, sizeof value) < 0 || copy_to_user(ulen, &len, sizeof len) < 0)
        return -EFAULT;
    return 0;
}

/* The file behind `fd` if it is an AF_UNIX socket (referenced), else NULL. */
static struct file *unix_fd(uint64_t fd) {
    struct file *f = fd_get((int)fd);
    if (f && unix_is_socket(f))
        return f;
    if (f)
        file_put(f);
    return NULL;
}

/* AF_UNIX descriptors are handled by unix.c: `fn(file, ...)`. */
#define UNIX_CALL(fd, fn, ...)                                                                           \
    do {                                                                                                 \
        struct file *uf_ = unix_fd(fd);                                                                  \
        if (uf_) {                                                                                       \
            int64_t r_ = fn(uf_, __VA_ARGS__);                                                           \
            file_put(uf_);                                                                               \
            return r_;                                                                                   \
        }                                                                                                \
    } while (0)

SYSCALL(socket) {
    UNUSED_ARGS();
    int domain = (int)a1, type = (int)a2 & 0xf;
    if (domain == AF_UNIX)
        return unix_socket(a2, a3);
    if (domain == AF_NETLINK)
        return netlink_socket(a2, a3);
    if (domain != AF_INET)
        return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -EPROTONOSUPPORT;
    uint64_t f = spin_lock_irqsave(&net_lock);
    struct sock *s = sock_alloc(type == SOCK_STREAM ? 1 : 2);
    net_unlock(f);
    if (!s)
        return -ENOMEM;
    int fd = sock_file(s, (a2 & SOCK_NONBLOCK) ? O_NONBLOCK : 0, a2 & SOCK_CLOEXEC);
    if (fd < 0) {
        f = spin_lock_irqsave(&net_lock);
        s->file_open = false;
        sock_free_if_done(s);
        net_unlock(f);
    }
    return fd;
}

SYSCALL(connect) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_connect, a2, a3);
    NL_CALL(a1, 0); /* talking to the kernel: nothing to connect */
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int64_t r = read_addr(a2, a3, &ip, &port);
    if (r == 0 && !netns_is_initial(netns_current()))
        r = -ENETUNREACH; /* a private network namespace: no IP here at all */
    if (r == 0) {
        if (!ip)
            ip = IP_LOOPBACK; /* 0.0.0.0 means this host, as on Linux */
        uint64_t f = spin_lock_irqsave(&net_lock);
        if (s->type == 2) {
            /* Fixes the local address too, so getsockname tells which
             * address the route uses (a common way to learn one's own IP). */
            uint32_t src = s->lip ? s->lip : ip_source_for(ip);
            if (!src) {
                r = -ENETUNREACH;
            } else {
                s->lip = src;
                s->rip = ip;
                s->rport = port;
                if (!s->lport)
                    s->lport = sock_ephemeral_port();
            }
        } else if (s->state != TCP_CLOSED) {
            r = s->state == TCP_SYN_SENT ? -EALREADY : -EISCONN;
        } else {
            s->rip = ip;
            s->rport = port;
            r = tcp_connect(s);
        }
        net_unlock(f);
        if (r == 0 && s->type == 1) {
            /* Over loopback the handshake is already done here (net_unlock
             * delivered it), but a non-blocking connect still answers
             * EINPROGRESS, as Linux does; POLLOUT then reports at once. */
            if (file->flags & O_NONBLOCK) {
                r = -EINPROGRESS;
            } else {
                r = sock_wait(s, 2, false);
                f = spin_lock_irqsave(&net_lock);
                if (r == 0 && s->error) {
                    r = -s->error;
                    s->error = 0; /* reported: SO_ERROR reads 0 afterwards */
                }
                net_unlock(f);
            }
        }
    }
    file_put(file);
    return r;
}

/* Whether a socket other than `s` already owns `port` on an address that
 * overlaps `ip` (0 = all of them). For TCP only listeners count: connections
 * keep using the port of the listener they came from, and SO_REUSEADDR's
 * "rebind while old connections linger" is therefore always granted. */
static bool port_taken(struct sock *s, uint32_t ip, uint16_t port) {
    for (struct sock *o = all_socks; o; o = o->next)
        if (o != s && o->type == s->type && o->lport == port && (!o->lip || !ip || o->lip == ip) &&
            (s->type == 2 || o->state == TCP_LISTEN))
            return true;
    return false;
}

SYSCALL(bind) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_bind, a2, a3);
    NL_CALL(a1, netlink_bind(nf_, a2, a3));
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int64_t r = read_addr(a2, a3, &ip, &port);
    if (r == 0 && !netns_is_initial(netns_current()))
        r = -EADDRNOTAVAIL;
    if (r == 0) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        if (ip && ip != 0xffffffff && !ip_is_local(ip))
            r = -EADDRNOTAVAIL;
        else if (port && port_taken(s, ip, port))
            r = -EADDRINUSE;
        if (r == 0) {
            s->lip = ip == 0xffffffff ? 0 : ip; /* a broadcast bind receives everything */
            s->lport = port ? port : sock_ephemeral_port();
        }
        net_unlock(f);
    }
    file_put(file);
    return r;
}

SYSCALL(listen) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_listen, (int)a2);
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    int64_t r = 0;
    uint64_t f = spin_lock_irqsave(&net_lock);
    int backlog = (int)a2;
    if (s->type != 1)
        r = -EOPNOTSUPP;
    else if (s->state != TCP_CLOSED && s->state != TCP_LISTEN)
        r = -EINVAL;
    else if (s->lport && port_taken(s, s->lip, s->lport))
        r = -EADDRINUSE; /* two sockets bound to one port; the first to listen wins */
    else {
        if (!s->lport)
            s->lport = sock_ephemeral_port(); /* listen() without bind(), as Linux allows */
        s->state = TCP_LISTEN;
        s->backlog = backlog <= 0 || backlog > SOCK_BACKLOG ? SOCK_BACKLOG : backlog;
    }
    net_unlock(f);
    file_put(file);
    return r;
}

static int64_t do_accept(uint64_t fd, uint64_t uaddr, uint64_t ulen, uint64_t flags) {
    UNIX_CALL(fd, unix_accept, uaddr, ulen, flags);
    struct file *file;
    struct sock *s = fd_sock((int)fd, &file);
    if (!s)
        return -ENOTSOCK;
    int64_t r;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        if (s->state != TCP_LISTEN) {
            net_unlock(f);
            r = -EINVAL;
            break;
        }
        if (s->accept_count) {
            struct sock *c = s->accept_q[0];
            memmove(s->accept_q, s->accept_q + 1, sizeof(struct sock *) * (size_t)--s->accept_count);
            c->listener = NULL;
            net_unlock(f);
            r = sock_file(c, (flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0, flags & SOCK_CLOEXEC);
            if (r >= 0)
                write_addr(uaddr, ulen, c->rip, c->rport);
            break;
        }
        net_unlock(f);
        r = sock_wait(s, 3, file->flags & O_NONBLOCK);
        if (r < 0)
            break;
    }
    file_put(file);
    return r;
}

SYSCALL(accept) {
    UNUSED_ARGS();
    return do_accept(a1, a2, a3, 0);
}

SYSCALL(accept4) {
    UNUSED_ARGS();
    return do_accept(a1, a2, a3, a4);
}

#define IO_CHUNK 65536

static int64_t unix_sendto(struct file *f, uint64_t ubuf, uint64_t len, uint64_t flags, uint64_t uname,
                           uint64_t namelen) {
    struct linux_iovec iov = {ubuf, len};
    return unix_sendmsg(f, &iov, 1, uname, (uint32_t)namelen, 0, 0, flags);
}

static int64_t unix_recvfrom(struct file *f, uint64_t ubuf, uint64_t len, uint64_t flags, uint64_t uname,
                             uint64_t ulen) {
    struct linux_iovec iov = {ubuf, len};
    uint32_t namelen = 0;
    uint64_t controllen = 0;
    int msg_flags = 0;
    if (uname && ulen && copy_from_user(&namelen, ulen, sizeof namelen) < 0)
        return -EFAULT;
    int64_t r = unix_recvmsg(f, &iov, 1, ulen ? uname : 0, &namelen, 0, &controllen, &msg_flags, flags);
    if (r >= 0 && uname && ulen && copy_to_user(ulen, &namelen, sizeof namelen) < 0)
        r = -EFAULT;
    return r;
}

/* sendmsg/recvmsg on AF_UNIX: the whole iovec array, names and control data
 * (SCM_RIGHTS) go to unix.c, and the lengths and flags it reports are
 * written back into the caller's msghdr. */
static int64_t unix_msg_io(struct file *f, uint64_t umsg, uint64_t flags, bool send) {
    struct linux_msghdr m;
    if (copy_from_user(&m, umsg, sizeof m) < 0)
        return -EFAULT;
    if (m.iovlen > 1024)
        return -EMSGSIZE;
    struct linux_iovec *iov = kmalloc(m.iovlen ? m.iovlen * sizeof *iov : 1);
    if (!iov)
        return -ENOMEM;
    int64_t r = copy_from_user(iov, m.iov, m.iovlen * sizeof *iov) < 0 ? -EFAULT : 0;
    if (r == 0 && send) {
        r = unix_sendmsg(f, iov, m.iovlen, m.name, m.namelen, m.control, m.controllen, flags);
    } else if (r == 0) {
        uint32_t namelen = m.namelen;
        uint64_t controllen = m.controllen;
        int msg_flags = 0;
        r = unix_recvmsg(f, iov, m.iovlen, m.name, &namelen, m.control, &controllen, &msg_flags, flags);
        if (r >= 0 && (copy_to_user(umsg + offsetof(struct linux_msghdr, namelen), &namelen, sizeof namelen) < 0 ||
                       copy_to_user(umsg + offsetof(struct linux_msghdr, controllen), &controllen,
                                    sizeof controllen) < 0 ||
                       copy_to_user(umsg + offsetof(struct linux_msghdr, flags), &msg_flags, sizeof msg_flags) < 0))
            r = -EFAULT;
    }
    kfree(iov);
    return r;
}

SYSCALL(sendto) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_sendto, a2, a3, a4, a5, a6);
    NL_CALL(a1, nl_sendto(nf_, a2, a3));
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t r = 0;
    if (a5 && s->type == 2) {
        r = read_addr(a5, a6, &ip, &port);
        if (r == 0 && !ip)
            ip = IP_LOOPBACK; /* 0.0.0.0 means this host */
    }
    size_t len = a3 < IO_CHUNK ? a3 : IO_CHUNK;
    uint8_t *kb = kmalloc(len ? len : 1);
    if (r == 0 && !kb)
        r = -ENOMEM;
    if (r == 0)
        r = copy_from_user(kb, a2, len);
    if (r == 0)
        r = sock_send(s, kb, len, (file->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), ip, port);
    if (r == -EPIPE && !(a4 & MSG_NOSIGNAL))
        proc_kill(proc_current(), SIGPIPE);
    kfree(kb);
    file_put(file);
    return r;
}

SYSCALL(recvfrom) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_recvfrom, a2, a3, a4, a5, a6);
    NL_CALL(a1, netlink_recv(nf_, a2, a3, a4, a5, a6, NULL));
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    size_t len = a3 < IO_CHUNK ? a3 : IO_CHUNK;
    uint8_t *kb = kmalloc(len ? len : 1);
    int64_t r = kb ? 0 : -ENOMEM;
    uint32_t from = s->rip;
    uint16_t fport = s->rport;
    if (r == 0)
        r = sock_recv(s, kb, len, (file->flags & O_NONBLOCK) || (a4 & MSG_DONTWAIT), &from, &fport, a4 & MSG_PEEK);
    if (r > 0 && copy_to_user(a2, kb, (size_t)r) < 0)
        r = -EFAULT;
    if (r >= 0 && a5)
        write_addr(a5, a6, from, fport);
    kfree(kb);
    file_put(file);
    return r;
}

static int64_t msg_io(uint64_t fd, uint64_t umsg, uint64_t flags, bool send) {
    UNIX_CALL(fd, unix_msg_io, umsg, flags, send);
    NL_CALL(fd, nl_msg_io(nf_, umsg, flags, send));
    struct linux_msghdr m;
    if (copy_from_user(&m, umsg, sizeof m) < 0)
        return -EFAULT;
    int64_t total = 0;
    for (uint64_t i = 0; i < m.iovlen && i < 64; i++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, m.iov + i * sizeof iov, sizeof iov) < 0)
            return total ? total : -EFAULT;
        if (!iov.iov_len)
            continue;
        int64_t n = send ? sys_sendto(fd, iov.iov_base, iov.iov_len, flags, i == 0 ? m.name : 0, m.namelen, NULL)
                         : sys_recvfrom(fd, iov.iov_base, iov.iov_len, flags, i == 0 ? m.name : 0,
                                        i == 0 && m.name ? umsg + 8 : 0, NULL);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint64_t)n < iov.iov_len || !send)
            break; /* datagrams: one message per call */
    }
    if (!send) {
        uint64_t zero = 0;
        copy_to_user(umsg + 40, &zero, 8); /* msg_controllen = 0 */
        int32_t fl = 0;
        copy_to_user(umsg + 48, &fl, 4);
    }
    return total;
}

SYSCALL(sendmsg) {
    UNUSED_ARGS();
    return msg_io(a1, a2, a3, true);
}

SYSCALL(recvmsg) {
    UNUSED_ARGS();
    return msg_io(a1, a2, a3, false);
}

/* Several messages in one call, each one's length written back next to it
 * (glibc's resolver sends its A and AAAA questions together this way). */
struct linux_mmsghdr {
    struct linux_msghdr hdr;
    uint32_t len;
};

SYSCALL(sendmmsg) {
    UNUSED_ARGS();
    uint64_t n = (uint32_t)a3 > 64 ? 64 : (uint32_t)a3, done = 0;
    for (; done < n; done++) {
        uint64_t m = a2 + done * sizeof(struct linux_mmsghdr);
        int64_t r = msg_io(a1, m, a4, true);
        if (r < 0)
            return done ? (int64_t)done : r;
        uint32_t len = (uint32_t)r;
        if (copy_to_user(m + __builtin_offsetof(struct linux_mmsghdr, len), &len, sizeof len) < 0)
            return -EFAULT;
    }
    return (int64_t)done;
}

SYSCALL(shutdown) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_shutdown, a2);
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    uint64_t f = spin_lock_irqsave(&net_lock);
    if (a2 == 0 || a2 == 2)
        s->shut_rd = true;
    if ((a2 == 1 || a2 == 2) && s->type == 1)
        tcp_close(s);
    net_unlock(f);
    waitq_wake_all(&s->wq);
    file_put(file);
    return 0;
}

SYSCALL(getsockname) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_getname, a2, a3, false);
    NL_CALL(a1, netlink_getname(nf_, a2, a3));
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    /* The bound or connected address; 0.0.0.0 for a wildcard socket. */
    int r = write_addr(a2, a3, s->lip, s->lport);
    file_put(file);
    return r;
}

SYSCALL(getpeername) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_getname, a2, a3, true);
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    bool connected = s->type == 1 ? s->state >= TCP_ESTABLISHED : s->rip != 0;
    int r = connected ? write_addr(a2, a3, s->rip, s->rport) : -ENOTCONN;
    file_put(file);
    return r;
}

#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
#define TCP_MAXSEG 2

SYSCALL(setsockopt) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_setsockopt, a2, a3, a4, a5);
    NL_CALL(a1, 0); /* buffer sizes, groups, strict checking: all accepted */
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    int value = 0;
    if (a5 >= sizeof value && copy_from_user(&value, a4, sizeof value) < 0) {
        file_put(file);
        return -EFAULT;
    }
    /* Everything is accepted. SO_REUSEADDR needs no action (see port_taken)
     * and TCP_NODELAY none either: there is no Nagle delay, every write is
     * sent at once. They are remembered so getsockopt reports them back. */
    if (a2 == SOL_SOCKET && a3 == SO_REUSEADDR)
        s->reuseaddr = value != 0;
    else if (a2 == SOL_SOCKET && a3 == SO_KEEPALIVE)
        s->keepalive = value != 0;
    else if (a2 == IPPROTO_TCP && a3 == TCP_NODELAY)
        s->nodelay = value != 0;
    file_put(file);
    return 0;
}

SYSCALL(getsockopt) {
    UNUSED_ARGS();
    UNIX_CALL(a1, unix_getsockopt, a2, a3, a4, a5);
    NL_CALL(a1, nl_getsockopt(a2, a3, a4, a5));
    struct file *file;
    struct sock *s = fd_sock((int)a1, &file);
    if (!s)
        return -ENOTSOCK;
    int value = 0;
    if (a2 == SOL_SOCKET && a3 == SO_ERROR) {
        uint64_t f = spin_lock_irqsave(&net_lock);
        value = s->error;
        s->error = 0;
        net_unlock(f);
    } else if (a2 == SOL_SOCKET) {
        switch (a3) {
        case SO_TYPE: value = s->type; break;
        case SO_REUSEADDR: value = s->reuseaddr; break;
        case SO_KEEPALIVE: value = s->keepalive; break;
        case SO_SNDBUF: value = SOCK_TX_SIZE; break;
        case SO_RCVBUF: value = SOCK_RX_SIZE; break;
        case SO_ACCEPTCONN: value = s->type == 1 && s->state == TCP_LISTEN; break;
        case SO_DOMAIN: value = AF_INET; break;
        case SO_PROTOCOL: value = s->type == 1 ? 6 : 17; break;
        }
    } else if (a2 == IPPROTO_TCP && a3 == TCP_NODELAY) {
        value = s->nodelay;
    } else if (a2 == IPPROTO_TCP && a3 == TCP_MAXSEG) {
        value = s->mss ? s->mss : 536;
    }
    file_put(file);
    uint32_t len = sizeof value;
    if (copy_to_user(a4, &value, sizeof value) < 0 || copy_to_user(a5, &len, sizeof len) < 0)
        return -EFAULT;
    return 0;
}

SYSCALL(socketpair) {
    UNUSED_ARGS();
    if (a1 != AF_UNIX)
        return a1 == AF_INET ? -EOPNOTSUPP : -EAFNOSUPPORT;
    return unix_socketpair(a2, a3, a4);
}
