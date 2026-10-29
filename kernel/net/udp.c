#include <kernel/lib.h>
#include <kernel/mm.h>
#include "sock.h"

struct udp_hdr {
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

static uint16_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, size_t len) {
    uint32_t s = (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + proto + (uint32_t)len;
    return (uint16_t)((s & 0xffff) + (s >> 16));
}

int udp_send_raw(uint32_t src_ip, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    /* Static, not on the stack: a loopback datagram may be 64 KiB, and
     * net_lock (held) keeps it to one user at a time. */
    static uint8_t buf[LO_MTU];
    if (len + sizeof(struct udp_hdr) + 20 > ip_mtu_for(dst))
        return -EMSGSIZE;
    struct udp_hdr *h = (struct udp_hdr *)buf;
    h->sport = htons(sport);
    h->dport = htons(dport);
    h->len = htons((uint16_t)(sizeof *h + len));
    h->csum = 0;
    memcpy(buf + sizeof *h, data, len);
    h->csum = net_checksum(buf, sizeof *h + len, pseudo_sum(src_ip, dst, 17, sizeof *h + len));
    if (!h->csum)
        h->csum = 0xffff;
    return ip_send(src_ip, dst, 17, buf, sizeof *h + len) < 0 ? -ENOBUFS : 0;
}

int udp_sendto(struct sock *s, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    uint32_t src = s->lip ? s->lip : ip_source_for(dst);
    if (!src)
        return -ENETUNREACH;
    if (!s->lport)
        s->lport = sock_ephemeral_port();
    int r = udp_send_raw(src, s->lport, dst, dport, data, len);
    return r < 0 ? r : (int)len;
}

void udp_input(struct netif *nif, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *h = (const void *)seg;
    size_t ulen = ntohs(h->len);
    if (ulen < sizeof *h || ulen > len)
        return;
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    const uint8_t *data = seg + sizeof *h;
    size_t dlen = ulen - sizeof *h;
    if (dport == 68 && !net_is_loopback(nif)) {
        dhcp_input(nif, data, dlen);
        return;
    }
    struct sock *s = sock_find(2, dst, dport, src, sport);
    if (!s || s->q_count == UDP_QUEUE)
        return;
    uint8_t *copy = kmalloc(dlen ? dlen : 1);
    if (!copy)
        return;
    memcpy(copy, data, dlen);
    struct dgram *d = &s->q[(s->q_head + s->q_count) % UDP_QUEUE];
    d->src = src;
    d->sport = sport;
    d->len = (uint16_t)dlen;
    d->data = copy;
    s->q_count++;
    waitq_wake_all(&s->wq);
}
