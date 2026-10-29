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

/* ---- ARP cache with one parked packet per unresolved entry ---- */

#define ARP_ENTRIES 32
static struct arp_entry {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
    uint64_t requested_ms;
    uint8_t *pending; /* IP packet waiting for resolution */
    size_t pending_len;
} arp[ARP_ENTRIES];

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

void net_unlock(uint64_t flags) {
    lo_drain();
    spin_unlock_irqrestore(&net_lock, flags);
}

static int eth_send(struct netif *nif, const uint8_t dst[6], uint16_t type, const void *payload, size_t len) {
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
    kfree(free_slot->pending);
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
        if (e->pending) {
            eth_send(nif, e->mac, ETH_IPV4, e->pending, e->pending_len);
            kfree(e->pending);
            e->pending = NULL;
        }
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
    *lo_tail = p;
    lo_tail = &p->next;
    lo_queued++;
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
    /* Park the packet (latest wins) and ask who has `hop`. */
    kfree(e->pending);
    e->pending = kmalloc(total);
    if (e->pending) {
        memcpy(e->pending, pkt, total);
        e->pending_len = total;
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

static void net_thread(void *arg) {
    (void)arg;
    bool announced = false;
    for (;;) {
        for (int i = 0; i < nnetif; i++) {
            struct netif *nif = netifs[i];
            uint64_t f = spin_lock_irqsave(&net_lock);
            nif->poll(nif);
            dhcp_tick(nif, now_ms());
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
        thread_sleep_ms(1); /* one scheduler tick (4 ms) */
    }
}

void net_init(void) {
    e1000_init();
    if (!nnetif)
        kprintf("net: no supported network adapter, loopback only\n");
    for (int i = 0; i < nnetif; i++)
        dhcp_start(netifs[i]);
    /* Started even without a NIC: loopback TCP needs its timers too. */
    KASSERT(thread_create("net", net_thread, NULL));
}
