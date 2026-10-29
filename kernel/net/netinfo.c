#include <kernel/lib.h>
#include <kernel/net.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

#include "../fs/synth.h"

/* What a program can learn about the network interfaces, the Linux way:
 * the SIOCGIF* ioctls on any socket (what ifconfig and the desktop's tray
 * use), and /proc/net/dev and /proc/net/route for counters and the default
 * gateway. Only eth0 (the first NIC) and lo exist. */

#define SIOCGIFNAME    0x8910
#define SIOCGIFCONF    0x8912
#define SIOCGIFFLAGS   0x8913
#define SIOCGIFADDR    0x8915
#define SIOCGIFBRDADDR 0x8919
#define SIOCGIFNETMASK 0x891b
#define SIOCGIFMTU     0x8921
#define SIOCGIFHWADDR  0x8927
#define SIOCGIFINDEX   0x8933

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

struct linux_sockaddr {
    uint16_t family;
    uint8_t data[14];
};

struct linux_ifreq {
    char name[16];
    union {
        struct linux_sockaddr addr;
        int16_t flags;
        int32_t ivalue;
        uint8_t pad[24];
    };
};

struct linux_ifconf {
    int32_t len;
    int32_t pad;
    uint64_t buf;
};

/* A consistent copy of one interface, taken under the stack's lock. */
struct ifsnap {
    char name[8];
    int index;
    bool loopback, up, running;
    uint8_t mac[6];
    uint32_t ip, mask, gw;
    uint64_t rx, tx;
};

static bool snap(const char *name, struct ifsnap *s) {
    memset(s, 0, sizeof *s);
    if (!strcmp(name, "lo")) {
        memcpy(s->name, "lo", 3);
        s->index = 1;
        s->loopback = s->up = s->running = true;
        s->ip = IP_LOOPBACK;
        s->mask = 0xff000000u;
        return true;
    }
    struct netif *nif = net_default();
    if (!nif || strcmp(name, nif->name))
        return false;
    uint64_t f = spin_lock_irqsave(&net_lock);
    memcpy(s->name, nif->name, sizeof s->name);
    s->index = 2;
    s->up = true;
    /* "running" is Linux's word for a live link: the cable is in */
    s->running = nif->link ? nif->link(nif) : true;
    memcpy(s->mac, nif->mac, 6);
    if (nif->configured) {
        s->ip = nif->ip;
        s->mask = nif->mask;
        s->gw = nif->gw;
    }
    s->rx = nif->rx_packets;
    s->tx = nif->tx_packets;
    net_unlock(f);
    return true;
}

static void put_in(struct linux_sockaddr *sa, uint32_t ip) {
    memset(sa, 0, sizeof *sa);
    sa->family = 2; /* AF_INET */
    uint32_t be = htonl(ip);
    memcpy(sa->data + 2, &be, 4); /* after the port */
}

static int64_t ifconf(uint64_t arg) {
    struct linux_ifconf c;
    if (copy_from_user(&c, arg, sizeof c) < 0)
        return -EFAULT;
    const char *names[2] = {"lo", net_default() ? net_default()->name : NULL};
    int32_t used = 0;
    for (int i = 0; i < 2; i++) {
        struct ifsnap s;
        if (!names[i] || !snap(names[i], &s))
            continue;
        /* a NULL buffer asks only how much room the list needs */
        if (c.buf && used + (int32_t)sizeof(struct linux_ifreq) <= c.len) {
            struct linux_ifreq r;
            memset(&r, 0, sizeof r);
            memcpy(r.name, s.name, sizeof s.name);
            put_in(&r.addr, s.ip);
            if (copy_to_user(c.buf + (uint64_t)used, &r, sizeof r) < 0)
                return -EFAULT;
        } else if (c.buf) {
            break;
        }
        used += (int32_t)sizeof(struct linux_ifreq);
    }
    c.len = used;
    return copy_to_user(arg, &c, sizeof c) < 0 ? -EFAULT : 0;
}

int64_t net_if_ioctl(uint64_t req, uint64_t arg) {
    if (req == SIOCGIFCONF)
        return ifconf(arg);
    if (req != SIOCGIFNAME && req != SIOCGIFFLAGS && req != SIOCGIFADDR && req != SIOCGIFBRDADDR &&
        req != SIOCGIFNETMASK && req != SIOCGIFMTU && req != SIOCGIFHWADDR && req != SIOCGIFINDEX)
        return -ENOTTY;
    struct linux_ifreq r;
    if (copy_from_user(&r, arg, sizeof r) < 0)
        return -EFAULT;
    r.name[15] = 0;
    struct ifsnap s;
    if (req == SIOCGIFNAME) {
        const char *n = r.ivalue == 1 ? "lo" : r.ivalue == 2 && net_default() ? net_default()->name : NULL;
        if (!n || !snap(n, &s))
            return -ENODEV;
        memset(r.name, 0, sizeof r.name);
        memcpy(r.name, s.name, sizeof s.name);
        return copy_to_user(arg, &r, sizeof r) < 0 ? -EFAULT : 0;
    }
    if (!snap(r.name, &s))
        return -ENODEV;
    switch (req) {
    case SIOCGIFFLAGS:
        r.flags = (int16_t)((s.up ? IFF_UP : 0) | (s.running ? IFF_RUNNING : 0) |
                            (s.loopback ? IFF_LOOPBACK : IFF_BROADCAST | IFF_MULTICAST));
        break;
    case SIOCGIFADDR:
        if (!s.ip)
            return -EADDRNOTAVAIL; /* no lease yet */
        put_in(&r.addr, s.ip);
        break;
    case SIOCGIFNETMASK:
        if (!s.ip)
            return -EADDRNOTAVAIL;
        put_in(&r.addr, s.mask);
        break;
    case SIOCGIFBRDADDR:
        if (!s.ip)
            return -EADDRNOTAVAIL;
        put_in(&r.addr, (s.ip & s.mask) | ~s.mask);
        break;
    case SIOCGIFMTU:
        r.ivalue = s.loopback ? LO_MTU : ETH_MTU;
        break;
    case SIOCGIFHWADDR:
        memset(&r.addr, 0, sizeof r.addr);
        r.addr.family = s.loopback ? 772 : 1; /* ARPHRD_LOOPBACK, ARPHRD_ETHER */
        memcpy(r.addr.data, s.mac, 6);
        break;
    case SIOCGIFINDEX:
        r.ivalue = s.index;
        break;
    }
    return copy_to_user(arg, &r, sizeof r) < 0 ? -EFAULT : 0;
}

/* ---- /proc/net ---- */

int procnet_dev(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "Inter-|   Receive                                                |  Transmit\n");
    sb_printf(b, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop "
                 "fifo colls carrier compressed\n");
    const char *names[2] = {"lo", net_default() ? net_default()->name : NULL};
    for (int i = 0; i < 2; i++) {
        struct ifsnap s;
        if (!names[i] || !snap(names[i], &s))
            continue;
        /* the stack counts packets, not bytes */
        sb_printf(b, "%6s: %8u %7lu    0    0    0     0          0         0 %8u %7lu    0    0    0     0       0"
                     "          0\n",
                  s.name, 0, s.rx, 0, s.tx);
    }
    return 0;
}

/* Addresses as Linux prints them: the network-order bytes read as a
 * little-endian word, in hex. */
static uint32_t route_hex(uint32_t ip) { return htonl(ip); }

int procnet_route(struct snode *n, struct sbuf *b) {
    (void)n;
    sb_printf(b, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    struct netif *nif = net_default();
    struct ifsnap s;
    if (!nif || !snap(nif->name, &s) || !s.ip)
        return 0;
    if (s.gw) /* RTF_UP | RTF_GATEWAY */
        sb_printf(b, "%s\t%08X\t%08X\t0003\t0\t0\t0\t%08X\t0\t0\t0\n", s.name, 0u, route_hex(s.gw), 0u);
    sb_printf(b, "%s\t%08X\t%08X\t0001\t0\t0\t0\t%08X\t0\t0\t0\n", s.name, route_hex(s.ip & s.mask), 0u,
              route_hex(s.mask));
    return 0;
}

/* Interface number `index` (1: lo, 2: the network adapter), for netlink. */
bool net_if_get(int index, struct net_ifinfo *out) {
    struct ifsnap s;
    struct netif *nif = net_default();
    if (index == 1 ? !snap("lo", &s) : index == 2 ? !nif || !snap(nif->name, &s) : true)
        return false;
    memset(out, 0, sizeof *out);
    memcpy(out->name, s.name, sizeof out->name);
    out->index = s.index;
    out->loopback = s.loopback;
    out->up = s.up;
    out->running = s.running;
    memcpy(out->mac, s.mac, 6);
    out->ip = s.ip;
    out->mask = s.mask;
    out->gw = s.gw;
    return true;
}
