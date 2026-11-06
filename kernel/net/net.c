#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/vfs.h>

spinlock_t net_lock = SPINLOCK_INIT;

#define MAX_NETIF 4
static struct netif *netifs[MAX_NETIF];
static int nnetif;

#define ETH_ARP  0x0806
#define ETH_IPV4 0x0800
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

struct eth_hdr {
    uint8_t dst[6], src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr {
    uint8_t ver_ihl, tos;
    uint16_t total_len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

/* ---- ARP cache, with the packets waiting for each unresolved entry ---- */

#define ARP_ENTRIES 32
/* Several connections opened at once (a browser, or two downloads) each send their SYN before the router's address
 * is known: with one packet kept, all but the last were lost and waited a whole second to be sent again. */
#define ARP_PENDING 8
static struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
    uint64_t requested_ms;
    uint8_t *pending[ARP_PENDING]; /* IP packets waiting for resolution, oldest first */
    size_t pending_len[ARP_PENDING];
    int n_pending;
} arp[ARP_ENTRIES];

static void arp_drop_pending(struct arp_entry *e) {
    for (int i = 0; i < e->n_pending; i++)
        kfree(e->pending[i]);
    e->n_pending = 0;
}

static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static uint64_t now_ms(void) { return time_monotonic_ns() / 1000000ULL; }

/* The Internet checksum of `data`, with `sum` (16-bit words as numbers, e.g.
 * a pseudo-header) added in. The one's complement sum does not depend on
 * byte order (RFC 1071), so the data is summed as little-endian 64-bit words
 * (carries wrap around: 2^64 = 1 modulo 0xffff) and byte-swapped once at
 * the end; four times fewer steps than 16-bit words, which shows on
 * loopback where every byte is summed on its way out. */
uint16_t net_checksum(const void *data, size_t len, uint32_t sum) {
    const uint8_t *p = data;
    uint64_t acc = 0;
    for (; len >= 8; p += 8, len -= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        acc += v;
        acc += acc < v; /* end-around carry */
    }
    acc = (acc & 0xffffffff) + (acc >> 32); /* room for the tail below */
    for (; len >= 2; p += 2, len -= 2)
        acc += (uint64_t)(p[0] | p[1] << 8);
    if (len)
        acc += p[0];
    while (acc >> 16)
        acc = (acc & 0xffff) + (acc >> 16);
    uint64_t total = (uint64_t)sum + (((acc & 0xff) << 8) | (acc >> 8));
    while (total >> 16)
        total = (total & 0xffff) + (total >> 16);
    return htons((uint16_t)~total);
}

void net_register(struct netif *nif) {
    if (nnetif < MAX_NETIF)
        netifs[nnetif++] = nif;
}

struct netif *net_default(void) { return nnetif ? netifs[0] : NULL; }

/* ---- loopback ("lo") ----
 *
 * Packets for 127.0.0.0/8 or for one of our own addresses never reach a NIC.
 * ip_send appends them to lo_queue; net_unlock delivers the queue through
 * ip_input before it releases net_lock. Delivering later rather than inside
 * ip_send matters: the sender is in the middle of tcp_output, tcp_timers'
 * walk of the socket list or similar, and a synchronous delivery would run
 * the receiving side (which answers with ACKs, which run the sender again...)
 * on top of that half-updated state, recursing as deep as the conversation
 * is long. Queued, the drain is a flat loop: answers produced while it runs
 * are appended and handled in turn, in order, as on a real wire.
 *
 * Every holder of net_lock leaves through net_unlock, so the queue is empty
 * whenever someone takes the lock: a send() returns with its data already in
 * the receiver's buffer and the ACKs processed. The drain is bounded by the
 * receivers' buffer space (a full window stops the senders). */

static struct netif lo_if = {
    .name = "lo", .ip = IP_LOOPBACK, .mask = 0xff000000, .configured = true,
};

struct lo_pkt {
    struct lo_pkt *next;
    size_t len;
    uint8_t data[];
};

#define LO_QUEUE_MAX 4096 /* packets; only a runaway sender gets near it */
int lo_chaos; /* (the fault injector for tests: see lo_send) */
static unsigned lo_chaos_n;
static struct lo_pkt *lo_held;
static struct lo_pkt *lo_head, **lo_tail = &lo_head;
static int lo_queued;

static void ip_input(struct netif *nif, const uint8_t *p, size_t len);

bool net_is_loopback(const struct netif *nif) { return nif == &lo_if; }

bool ip_is_local(uint32_t addr) {
    if ((addr >> 24) == 127)
        return true;
    for (int i = 0; i < nnetif; i++)
        if (netifs[i]->configured && netifs[i]->ip && netifs[i]->ip == addr)
            return true;
    return false;
}

uint32_t ip_source_for(uint32_t dst) {
    if ((dst >> 24) == 127)
        return IP_LOOPBACK;
    if (ip_is_local(dst))
        return dst; /* talking to our own NIC address: from it, as Linux does */
    struct netif *nif = net_default();
    return nif && nif->configured ? nif->ip : 0;
}

size_t ip_mtu_for(uint32_t dst) { return ip_is_local(dst) ? LO_MTU : ETH_MTU; }

static void lo_drain(void) {
    if (lo_held && !lo_head) { /* nothing came after the late one: it goes now */
        lo_head = lo_held;
        lo_tail = &lo_held->next;
        lo_held = NULL;
        lo_queued++;
    }
    while (lo_head) {
        struct lo_pkt *p = lo_head;
        lo_head = p->next;
        if (!lo_head)
            lo_tail = &lo_head;
        lo_queued--;
        lo_if.rx_packets++;
        ip_input(&lo_if, p->data, p->len);
        kfree(p);
    }
}

int tcp_flush_deferred(void);

void net_unlock(uint64_t flags) {
    /* the readers woken and the ACKs sent for the data taken in meanwhile; an ACK over loopback is delivered in turn */
    do
        lo_drain();
    while (tcp_flush_deferred());
    spin_unlock_irqrestore(&net_lock, flags);
}

static uint64_t last_traffic_ms;

static int eth_send(struct netif *nif, const uint8_t dst[6], uint16_t type, const void *payload, size_t len) {
    if (!nif->interrupts && now_ms() - last_traffic_ms > 100)
        net_kick(); /* an answer will come: the ring is looked at often from now (a card without interrupts) */
    last_traffic_ms = now_ms();
    uint8_t frame[ETH_FRAME_MAX];
    if (len > ETH_MTU)
        return -1;
    struct eth_hdr *h = (struct eth_hdr *)frame;
    memcpy(h->dst, dst, 6);
    memcpy(h->src, nif->mac, 6);
    h->type = htons(type);
    memcpy(frame + sizeof *h, payload, len);
    return nif->tx(nif, frame, sizeof *h + len);
}

static void arp_send(struct netif *nif, uint16_t op, const uint8_t tha[6], uint32_t tpa) {
    struct arp_pkt a = {
        .htype = htons(1), .ptype = htons(ETH_IPV4), .hlen = 6, .plen = 4, .op = htons(op),
        .spa = htonl(nif->ip), .tpa = htonl(tpa),
    };
    memcpy(a.sha, nif->mac, 6);
    memcpy(a.tha, tha, 6);
    eth_send(nif, op == 1 ? broadcast_mac : tha, ETH_ARP, &a, sizeof a);
}

static struct arp_entry *arp_lookup(uint32_t ip, bool create) {
    struct arp_entry *free_slot = NULL;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp[i].ip == ip && (arp[i].valid || arp[i].requested_ms))
            return &arp[i];
        if (!free_slot && !arp[i].valid && !arp[i].requested_ms)
            free_slot = &arp[i];
    }
    if (!create)
        return NULL;
    if (!free_slot)
        free_slot = &arp[ip % ARP_ENTRIES]; /* evict */
    arp_drop_pending(free_slot);
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->ip = ip;
    return free_slot;
}

static void arp_input(struct netif *nif, const uint8_t *p, size_t len) {
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const void *)p;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    struct arp_entry *e = arp_lookup(spa, tpa == nif->ip);
    if (e) {
        memcpy(e->mac, a->sha, 6);
        e->valid = true;
        for (int i = 0; i < e->n_pending; i++)
            eth_send(nif, e->mac, ETH_IPV4, e->pending[i], e->pending_len[i]);
        arp_drop_pending(e);
    }
    if (ntohs(a->op) == 1 && nif->configured && tpa == nif->ip)
        arp_send(nif, 2, a->sha, spa);
}

static uint16_t ip_ident;

static void ip_header(struct ip_hdr *ip, uint32_t src, uint32_t dst, uint8_t proto, size_t len) {
    *ip = (struct ip_hdr){
        .ver_ihl = 0x45, .total_len = htons((uint16_t)(sizeof *ip + len)), .id = htons(ip_ident++),
        .frag = htons(0x4000) /* DF */, .ttl = 64, .proto = proto, .src = htonl(src), .dst = htonl(dst),
    };
    ip->csum = net_checksum(ip, sizeof *ip, 0);
}

/* Queues a packet for local delivery (see lo_drain). */
/* For tests (/proc/sys/net/ipv4/os_loopback_chaos = N, root only): over loopback, every Nth TCP segment carrying
 * data is lost and another held back behind the next one, so the stack's loss and reordering handling (resends,
 * segments kept ahead of a gap, SACK) is exercised where nothing else would lose a packet. 0: off. */
static int lo_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    if (len + sizeof(struct ip_hdr) > LO_MTU || lo_queued >= LO_QUEUE_MAX)
        return -1;
    struct lo_pkt *p = kmalloc(sizeof *p + sizeof(struct ip_hdr) + len);
    if (!p)
        return -1;
    ip_header((struct ip_hdr *)p->data, src, dst, proto, len);
    memcpy(p->data + sizeof(struct ip_hdr), payload, len);
    p->len = sizeof(struct ip_hdr) + len;
    p->next = NULL;
    const uint8_t *tp = payload;
    if (lo_chaos >= 4 && proto == PROTO_TCP && len >= 20 && len > (size_t)(tp[12] >> 4) * 4) {
        lo_chaos_n = lo_chaos_n * 1103515245u + 12345u; /* at random, one in N of each (not every Nth: a sender's */
        unsigned n = (lo_chaos_n >> 16) % (unsigned)lo_chaos; /* rounds could fall in step with that) */
        if (n == 0) { /* lost */
            kfree(p);
            return 0;
        }
        if (n == 1 && !lo_held) { /* late: after the next one */
            lo_held = p;
            return 0;
        }
    }
    *lo_tail = p;
    lo_tail = &p->next;
    lo_queued++;
    if (lo_held && lo_held != p) {
        struct lo_pkt *h = lo_held;
        lo_held = NULL;
        *lo_tail = h;
        lo_tail = &h->next;
        lo_queued++;
    }
    lo_if.tx_packets++;
    return 0;
}

int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    if (ip_is_local(dst))
        return lo_send(src, dst, proto, payload, len);
    struct netif *nif = net_default();
    if (!nif || len + sizeof(struct ip_hdr) > ETH_MTU)
        return -1;
    uint8_t pkt[ETH_MTU];
    struct ip_hdr *ip = (struct ip_hdr *)pkt;
    ip_header(ip, src, dst, proto, len);
    memcpy(pkt + sizeof *ip, payload, len);
    size_t total = sizeof *ip + len;

    if (dst == 0xffffffff || (nif->ip == 0 && proto == PROTO_UDP))
        return eth_send(nif, broadcast_mac, ETH_IPV4, pkt, total);
    uint32_t hop = (dst & nif->mask) == (nif->ip & nif->mask) ? dst : nif->gw;
    struct arp_entry *e = arp_lookup(hop, true);
    if (e->valid)
        return eth_send(nif, e->mac, ETH_IPV4, pkt, total);
    /* Park the packet (when full, the oldest gives way) and ask who has `hop`. */
    if (e->n_pending == ARP_PENDING) {
        kfree(e->pending[0]);
        memmove(&e->pending[0], &e->pending[1], sizeof e->pending[0] * (ARP_PENDING - 1));
        memmove(&e->pending_len[0], &e->pending_len[1], sizeof e->pending_len[0] * (ARP_PENDING - 1));
        e->n_pending--;
    }
    uint8_t *keep = kmalloc(total);
    if (keep) {
        memcpy(keep, pkt, total);
        e->pending[e->n_pending] = keep;
        e->pending_len[e->n_pending++] = total;
    }
    if (!e->requested_ms || now_ms() - e->requested_ms > 500) {
        e->requested_ms = now_ms();
        arp_send(nif, 1, (const uint8_t[6]){0}, hop);
    }
    return 0;
}

static void icmp_input(struct netif *nif, uint32_t src, uint32_t dst, const uint8_t *p, size_t len) {
    if (len < 8 || p[0] != 8) /* echo request only */
        return;
    uint8_t reply[ETH_MTU];
    if (len > sizeof reply)
        return;
    memcpy(reply, p, len);
    reply[0] = 0;
    reply[2] = reply[3] = 0;
    uint16_t c = net_checksum(reply, len, 0);
    memcpy(reply + 2, &c, 2);
    /* Answer from the address that was asked (a broadcast ping: from ours). */
    ip_send(ip_is_local(dst) ? dst : nif->ip, src, PROTO_ICMP, reply, len);
}

static void ip_input(struct netif *nif, const uint8_t *p, size_t len) {
    if (len < sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *ip = (const void *)p;
    size_t ihl = (size_t)(ip->ver_ihl & 0xf) * 4;
    size_t total = ntohs(ip->total_len);
    if ((ip->ver_ihl >> 4) != 4 || ihl < 20 || total > len || total < ihl)
        return;
    if (ntohs(ip->frag) & 0x3fff)
        return; /* fragments: not reassembled (we always send DF) */
    uint32_t src = ntohl(ip->src), dst = ntohl(ip->dst);
    bool for_us;
    if (nif == &lo_if)
        for_us = true; /* only ip_send puts packets here, and only local ones */
    else if ((dst >> 24) == 127 || (src >> 24) == 127)
        for_us = false; /* loopback addresses arriving on a wire are forged */
    else
        for_us = dst == nif->ip || dst == 0xffffffff || !nif->configured ||
                 (nif->mask && dst == (nif->ip | ~nif->mask));
    if (!for_us)
        return;
    const uint8_t *payload = p + ihl;
    size_t plen = total - ihl;
    switch (ip->proto) {
    case PROTO_ICMP: icmp_input(nif, src, dst, payload, plen); break;
    case PROTO_UDP: udp_input(nif, src, dst, payload, plen); break;
    case PROTO_TCP: tcp_input(nif, src, dst, payload, plen); break;
    }
}

void net_rx(struct netif *nif, const uint8_t *frame, size_t len) {
    if (len < sizeof(struct eth_hdr))
        return;
    const struct eth_hdr *h = (const void *)frame;
    uint16_t type = ntohs(h->type);
    if (type == ETH_ARP)
        arp_input(nif, frame + sizeof *h, len - sizeof *h);
    else if (type == ETH_IPV4)
        ip_input(nif, frame + sizeof *h, len - sizeof *h);
}

/* ---- the network thread ---- */

static void write_resolv_conf(struct netif *nif) {
    struct file *f;
    if (vfs_open(NULL, "/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644, &f) < 0)
        return;
    char buf[64];
    int n = ksnprintf(buf, sizeof buf, "nameserver %u.%u.%u.%u\n", nif->dns >> 24, (nif->dns >> 16) & 0xff,
                      (nif->dns >> 8) & 0xff, nif->dns & 0xff);
    file_write(f, buf, (size_t)n);
    file_put(f);
}

/* The kernel's log, sent to the network the computer is on as it is written (UDP broadcast to port 6666): only
 * when asked for at the start ("netlog" on the kernel command line: limine.conf), for seeing what happens on a
 * real computer with no serial port. Never otherwise: the log says what the system did, and broadcasting it on
 * someone's network unasked would tell anyone listening. */
bool netlog_wanted;
#define NETLOG_PORT 6666

const char *klog_text(unsigned long *len);
int udp_send_raw(uint32_t src_ip, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);

static void netlog_flush(struct netif *nif) {
    static unsigned long sent;
    unsigned long len;
    const char *t = klog_text(&len);
    if (len < sent) /* (the log dropped its oldest half: sent again from what is left) */
        sent = 0;
    for (int packets = 0; sent < len && packets < 16; packets++) {
        unsigned long n = len - sent > 1200 ? 1200 : len - sent;
        if (n == 1200) /* whole lines where it can */
            while (n > 200 && t[sent + n - 1] != '\n')
                n--;
        if (udp_send_raw(nif->ip, NETLOG_PORT, 0xffffffffu, NETLOG_PORT, t + sent, n) < 0)
            break;
        sent += n;
    }
}

static struct thread *net_thr;
static volatile bool net_kicked;

void net_kick(void) {
    net_kicked = true;
    if (net_thr)
        thread_wake(net_thr);
}

/* The network thread: takes in what the cards received and runs TCP's timers. Woken by a card's interrupt; a card
 * without (the 82540EM) is looked at every millisecond while traffic flows, every 10 ms when the line is quiet,
 * and at once when something is sent. (It used to look every 4 ms tick, whatever: a ceiling of 64 frames per
 * tick, about 24 MB/s, and an interrupt's worth of work 250 times a second for nothing.) */
static void net_thread(void *arg) {
    (void)arg;
    net_thr = thread_current();
    bool announced = false;
    for (;;) {
        bool polled_only = false;
        for (int i = 0; i < nnetif; i++) {
            struct netif *nif = netifs[i];
            uint64_t f = spin_lock_irqsave(&net_lock);
            if (nif->poll(nif) > 0)
                last_traffic_ms = now_ms();
            polled_only |= !nif->interrupts;
            dhcp_tick(nif, now_ms());
            if (netlog_wanted && nif->configured && !net_is_loopback(nif))
                netlog_flush(nif);
            net_unlock(f);
            if (nif->configured && !announced) {
                announced = true;
                write_resolv_conf(nif);
                kprintf("net: %s is %u.%u.%u.%u, gateway %u.%u.%u.%u, dns %u.%u.%u.%u\n", nif->name, nif->ip >> 24,
                        (nif->ip >> 16) & 0xff, (nif->ip >> 8) & 0xff, nif->ip & 0xff, nif->gw >> 24,
                        (nif->gw >> 16) & 0xff, (nif->gw >> 8) & 0xff, nif->gw & 0xff, nif->dns >> 24,
                        (nif->dns >> 16) & 0xff, (nif->dns >> 8) & 0xff, nif->dns & 0xff);
            }
        }
        uint64_t f = spin_lock_irqsave(&net_lock);
        tcp_timers(now_ms());
        net_unlock(f);
        uint64_t every_ms = polled_only && now_ms() - last_traffic_ms < 300 ? 1 : 10;
        if (!__atomic_exchange_n(&net_kicked, false, __ATOMIC_ACQ_REL))
            thread_block_until_ns(time_monotonic_ns() + every_ms * 1000000ULL);
    }
}

void net_init(void) {
    e1000_init();
    r8169_init();
    if (!nnetif)
        kprintf("net: no supported network adapter, loopback only\n");
    for (int i = 0; i < nnetif; i++)
        dhcp_start(netifs[i]);
    /* Started even without a NIC: loopback TCP needs its timers too. */
    KASSERT(thread_create("net", net_thread, NULL));
}
