#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/ns.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include <linux/syscall.h>

/* AF_NETLINK, NETLINK_ROUTE: how programs list network interfaces,
 * addresses and routes (Chromium, GLib's network monitor), and how
 * bubblewrap brings up the loopback of a new network namespace.
 *
 * Requests are answered at once: dumps from the interface table (in a
 * private network namespace, only "lo"), and changes acknowledged without
 * being applied (the stack has one configuration, set by DHCP). Each answer
 * is one datagram, as Linux sends a dump's messages together. */

#define NETLINK_ROUTE 0
#define NLMSG_ERROR 2
#define NLMSG_DONE 3
#define NLM_F_MULTI 2
#define NLM_F_ACK 4
#define NLM_F_DUMP 0x300
#define NLM_F_CAPPED 0x100
#define RTM_NEWLINK 16
#define RTM_GETLINK 18
#define RTM_NEWADDR 20
#define RTM_GETADDR 22
#define RTM_NEWROUTE 24
#define RTM_GETROUTE 26
#define IFLA_ADDRESS 1
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_OPERSTATE 16
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define RTA_DST 1
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PREFSRC 7
#define RTA_TABLE 15
#define IFF_UP 1
#define IFF_BROADCAST 2
#define IFF_LOOPBACK 8
#define IFF_RUNNING 0x40
#define IFF_MULTICAST 0x1000
#define IFF_LOWER_UP 0x10000
#define MSG_PEEK_ 2
#define MSG_TRUNC_ 0x20
#define MSG_DONTWAIT_ 0x40

struct nlmsghdr {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
};

struct ifinfomsg {
    uint8_t family, pad;
    uint16_t type;
    int32_t index;
    uint32_t flags, change;
};

struct ifaddrmsg {
    uint8_t family, prefixlen, flags, scope;
    uint32_t index;
};

struct rtmsg {
    uint8_t family, dst_len, src_len, tos, table, protocol, scope, type;
    uint32_t flags;
};

struct sockaddr_nl {
    uint16_t family, pad;
    uint32_t pid, groups;
};

struct packet {
    struct packet *next;
    size_t len;
    uint8_t data[];
};

struct nlsock {
    uint32_t portid; /* bound address (0: none yet) */
    uint32_t groups;
    struct packet *head, *tail;
    struct waitq wq;
    spinlock_t lock;
};

#define ALIGN4(n) (((n) + 3u) & ~3u)

/* A reply being built. */
struct out {
    uint8_t *buf;
    size_t len, cap;
    bool oom;
};

static void *out_put(struct out *o, size_t n) {
    n = ALIGN4(n);
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 1024;
        while (cap < o->len + n)
            cap *= 2;
        uint8_t *b = kmalloc(cap);
        if (!b) {
            o->oom = true;
            return NULL;
        }
        if (o->buf) {
            memcpy(b, o->buf, o->len);
            kfree(o->buf);
        }
        o->buf = b;
        o->cap = cap;
    }
    void *p = o->buf + o->len;
    memset(p, 0, n);
    o->len += n;
    return p;
}

static size_t msg_begin(struct out *o, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid) {
    size_t at = o->len;
    struct nlmsghdr *h = out_put(o, sizeof *h);
    if (h)
        *h = (struct nlmsghdr){0, type, flags, seq, pid};
    return at;
}

static void msg_end(struct out *o, size_t at) {
    if (!o->oom)
        ((struct nlmsghdr *)(o->buf + at))->len = (uint32_t)(o->len - at);
}

static void attr(struct out *o, uint16_t type, const void *data, size_t n) {
    uint16_t *a = out_put(o, 4 + n);
    if (!a)
        return;
    a[0] = (uint16_t)(4 + n);
    a[1] = type;
    memcpy(a + 2, data, n);
}

static void attr32(struct out *o, uint16_t type, uint32_t v) { attr(o, type, &v, 4); }

/* The interfaces this caller's network namespace has. */
static int interfaces(struct net_ifinfo *ifs, int max) {
    int n = 0;
    bool private_net = !netns_is_initial(netns_current());
    for (int i = 1; i <= 2 && n < max; i++)
        if ((i == 1 || !private_net) && net_if_get(i, &ifs[n]))
            n++;
    return n;
}

static int prefix_len(uint32_t mask) {
    int n = 0;
    for (; mask; mask <<= 1)
        n++;
    return n;
}

static void dump_links(struct out *o, const struct nlmsghdr *rq) {
    struct net_ifinfo ifs[2];
    int n = interfaces(ifs, 2);
    for (int i = 0; i < n; i++) {
        size_t at = msg_begin(o, RTM_NEWLINK, NLM_F_MULTI, rq->seq, rq->pid);
        struct ifinfomsg *m = out_put(o, sizeof *m);
        if (m) {
            m->type = ifs[i].loopback ? 772 : 1; /* ARPHRD_LOOPBACK, ARPHRD_ETHER */
            m->index = ifs[i].index;
            m->flags = (ifs[i].up ? IFF_UP : 0) | (ifs[i].running ? IFF_RUNNING | IFF_LOWER_UP : 0) |
                       (ifs[i].loopback ? IFF_LOOPBACK : IFF_BROADCAST | IFF_MULTICAST);
            m->change = 0xffffffffu;
        }
        attr(o, IFLA_IFNAME, ifs[i].name, strlen(ifs[i].name) + 1);
        attr32(o, IFLA_MTU, ifs[i].loopback ? 65536 : 1500);
        uint8_t oper = ifs[i].running ? 6 : 2; /* IF_OPER_UP, IF_OPER_DOWN */
        attr(o, IFLA_OPERSTATE, &oper, 1);
        attr(o, IFLA_ADDRESS, ifs[i].mac, 6);
        msg_end(o, at);
    }
}

static void dump_addrs(struct out *o, const struct nlmsghdr *rq) {
    struct net_ifinfo ifs[2];
    int n = interfaces(ifs, 2);
    for (int i = 0; i < n; i++) {
        if (!ifs[i].ip)
            continue;
        size_t at = msg_begin(o, RTM_NEWADDR, NLM_F_MULTI, rq->seq, rq->pid);
        struct ifaddrmsg *m = out_put(o, sizeof *m);
        if (m) {
            m->family = AF_INET;
            m->prefixlen = (uint8_t)prefix_len(ifs[i].mask);
            m->flags = 0x80; /* IFA_F_PERMANENT */
            m->scope = ifs[i].loopback ? 254 : 0; /* RT_SCOPE_HOST, RT_SCOPE_UNIVERSE */
            m->index = (uint32_t)ifs[i].index;
        }
        uint32_t ip = htonl(ifs[i].ip);
        attr(o, IFA_ADDRESS, &ip, 4);
        attr(o, IFA_LOCAL, &ip, 4);
        attr(o, IFA_LABEL, ifs[i].name, strlen(ifs[i].name) + 1);
        msg_end(o, at);
    }
}

static void route(struct out *o, const struct nlmsghdr *rq, uint32_t dst, int dst_len, uint32_t gw, uint32_t src,
                  int oif) {
    size_t at = msg_begin(o, RTM_NEWROUTE, NLM_F_MULTI, rq->seq, rq->pid);
    struct rtmsg *m = out_put(o, sizeof *m);
    if (m) {
        m->family = AF_INET;
        m->dst_len = (uint8_t)dst_len;
        m->table = 254; /* RT_TABLE_MAIN */
        m->protocol = gw ? 16 : 2; /* RTPROT_DHCP, RTPROT_KERNEL */
        m->scope = gw ? 0 : 253;   /* RT_SCOPE_UNIVERSE, RT_SCOPE_LINK */
        m->type = 1;               /* RTN_UNICAST */
    }
    attr32(o, RTA_TABLE, 254);
    if (dst_len) {
        uint32_t d = htonl(dst);
        attr(o, RTA_DST, &d, 4);
    }
    if (gw) {
        uint32_t g = htonl(gw);
        attr(o, RTA_GATEWAY, &g, 4);
    }
    if (src) {
        uint32_t s = htonl(src);
        attr(o, RTA_PREFSRC, &s, 4);
    }
    attr32(o, RTA_OIF, (uint32_t)oif);
    msg_end(o, at);
}

static void dump_routes(struct out *o, const struct nlmsghdr *rq) {
    struct net_ifinfo ifs[2];
    int n = interfaces(ifs, 2);
    for (int i = 0; i < n; i++) {
        if (ifs[i].loopback || !ifs[i].ip)
            continue;
        if (ifs[i].gw)
            route(o, rq, 0, 0, ifs[i].gw, 0, ifs[i].index);
        route(o, rq, ifs[i].ip & ifs[i].mask, prefix_len(ifs[i].mask), 0, ifs[i].ip, ifs[i].index);
    }
}

static void ack(struct out *o, const struct nlmsghdr *rq, int error) {
    size_t at = msg_begin(o, NLMSG_ERROR, NLM_F_CAPPED, rq->seq, rq->pid);
    int32_t *e = out_put(o, 4 + sizeof *rq);
    if (e) {
        e[0] = error;
        memcpy(e + 1, rq, sizeof *rq);
    }
    msg_end(o, at);
}

static void done(struct out *o, const struct nlmsghdr *rq) {
    size_t at = msg_begin(o, NLMSG_DONE, NLM_F_MULTI, rq->seq, rq->pid);
    int32_t *z = out_put(o, 4);
    if (z)
        *z = 0;
    msg_end(o, at);
}

static void queue(struct nlsock *s, struct out *o) {
    if (o->oom || !o->len) {
        kfree(o->buf);
        return;
    }
    struct packet *pk = kmalloc(sizeof *pk + o->len);
    if (pk) {
        pk->next = NULL;
        pk->len = o->len;
        memcpy(pk->data, o->buf, o->len);
        uint64_t f = spin_lock_irqsave(&s->lock);
        if (s->tail)
            s->tail->next = pk;
        else
            s->head = pk;
        s->tail = pk;
        spin_unlock_irqrestore(&s->lock, f);
        waitq_wake_all(&s->wq);
    }
    kfree(o->buf);
}

int64_t netlink_send(struct file *f, const void *buf, size_t len) {
    struct nlsock *s = f->vnode->data;
    const uint8_t *p = buf;
    size_t off = 0;
    while (off + sizeof(struct nlmsghdr) <= len) {
        struct nlmsghdr rq;
        memcpy(&rq, p + off, sizeof rq);
        if (rq.len < sizeof rq || off + rq.len > len)
            break;
        if (!rq.pid)
            rq.pid = s->portid;
        struct out o = {0};
        bool dump = (rq.flags & NLM_F_DUMP) == NLM_F_DUMP;
        if (dump && rq.type == RTM_GETLINK) {
            dump_links(&o, &rq);
            done(&o, &rq);
        } else if (dump && rq.type == RTM_GETADDR) {
            dump_addrs(&o, &rq);
            done(&o, &rq);
        } else if (dump && rq.type == RTM_GETROUTE) {
            dump_routes(&o, &rq);
            done(&o, &rq);
        } else if (dump) {
            done(&o, &rq); /* other tables: empty */
        } else if (rq.type < 16 || rq.type > 100) {
            ack(&o, &rq, -EOPNOTSUPP);
        } else if (rq.type % 4 == 2) {
            ack(&o, &rq, -EOPNOTSUPP); /* a single GET: not kept */
        } else {
            /* NEW, DEL, SET: the stack keeps its own configuration; bringing
             * up "lo" (bubblewrap) needs nothing more */
            if (rq.flags & NLM_F_ACK)
                ack(&o, &rq, 0);
        }
        queue(s, &o);
        off += ALIGN4(rq.len);
    }
    return (int64_t)len;
}

static bool has_packet(void *arg) {
    struct nlsock *s = arg;
    return __atomic_load_n(&s->head, __ATOMIC_ACQUIRE) != NULL;
}

int64_t netlink_recv(struct file *f, uint64_t ubuf, size_t len, uint64_t flags, uint64_t uname, uint64_t ulen,
                     int *msg_flags) {
    struct nlsock *s = f->vnode->data;
    while (!has_packet(s)) {
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT_))
            return -EAGAIN;
        if (waitq_wait_cond(&s->wq, has_packet, s) < 0)
            return -EINTR;
    }
    uint64_t lf = spin_lock_irqsave(&s->lock);
    struct packet *pk = s->head;
    if (pk && !(flags & MSG_PEEK_)) {
        s->head = pk->next;
        if (!s->head)
            s->tail = NULL;
    }
    spin_unlock_irqrestore(&s->lock, lf);
    if (!pk)
        return -EAGAIN;
    size_t n = pk->len < len ? pk->len : len;
    int64_t r = copy_to_user(ubuf, pk->data, n) < 0 ? -EFAULT : (int64_t)n;
    if (pk->len > len && msg_flags)
        *msg_flags |= MSG_TRUNC_;
    if (r >= 0 && (flags & MSG_TRUNC_))
        r = (int64_t)pk->len; /* the real size, so a caller can size its buffer */
    if (uname && ulen) {
        uint32_t have;
        struct sockaddr_nl from = {.family = AF_NETLINK};
        if (copy_from_user(&have, ulen, 4) == 0) {
            copy_to_user(uname, &from, have < sizeof from ? have : sizeof from);
            have = sizeof from;
            copy_to_user(ulen, &have, 4);
        }
    }
    if (!(flags & MSG_PEEK_))
        kfree(pk);
    return r;
}

int64_t netlink_bind(struct file *f, uint64_t uaddr, uint64_t len) {
    struct nlsock *s = f->vnode->data;
    struct sockaddr_nl a = {0};
    if (len < sizeof a || copy_from_user(&a, uaddr, sizeof a) < 0)
        return -EINVAL;
    if (a.family != AF_NETLINK)
        return -EINVAL;
    s->portid = a.pid ? a.pid : (uint32_t)pid_to_user(proc_current()->pid);
    s->groups = a.groups; /* multicast change notices: none are sent */
    return 0;
}

int64_t netlink_getname(struct file *f, uint64_t uaddr, uint64_t ulen) {
    struct nlsock *s = f->vnode->data;
    uint32_t have;
    if (copy_from_user(&have, ulen, 4) < 0)
        return -EFAULT;
    struct sockaddr_nl a = {.family = AF_NETLINK, .pid = s->portid, .groups = s->groups};
    if (copy_to_user(uaddr, &a, have < sizeof a ? have : sizeof a) < 0)
        return -EFAULT;
    have = sizeof a;
    return copy_to_user(ulen, &have, 4);
}

static int nl_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct nlsock *s = v->data;
    if (wq)
        *wq = &s->wq;
    return (has_packet(s) ? POLLIN : 0) | POLLOUT;
}

static void nl_release(struct vnode *v) {
    struct nlsock *s = v->data;
    while (s->head) {
        struct packet *n = s->head->next;
        kfree(s->head);
        s->head = n;
    }
    kfree(s);
}

static const struct vnode_ops nl_ops = {.poll = nl_poll, .release = nl_release};

bool netlink_is_socket(struct file *f) { return f->vnode->ops == &nl_ops; }

int64_t netlink_socket(uint64_t type, uint64_t proto) {
    if (proto != NETLINK_ROUTE)
        return -EPROTONOSUPPORT;
    if ((type & 0xf) != 3 && (type & 0xf) != 2) /* SOCK_RAW, SOCK_DGRAM */
        return -ESOCKTNOSUPPORT;
    struct nlsock *s = kzalloc(sizeof *s);
    struct vnode *v = s ? vnode_alloc(VN_SOCK, 0777, &nl_ops) : NULL;
    if (!v) {
        kfree(s);
        return -ENOMEM;
    }
    waitq_init(&s->wq);
    s->portid = (uint32_t)pid_to_user(proc_current()->pid);
    v->data = s;
    return vnode_install_fd(v, O_RDWR | ((type & SOCK_NONBLOCK) ? O_NONBLOCK : 0), type & SOCK_CLOEXEC);
}
