#include <kernel/lib.h>
#include <kernel/mm.h>
#include "sock.h"

/* TCP (RFC 793/9293) for a desktop: downloads from far away and local services
 * over loopback. Window scaling (RFC 7323) with a receive buffer that grows
 * while data streams in (to 4 MiB: a round trip's worth from across the world),
 * segments that arrive ahead of a gap kept and merged when it fills, SACK
 * (RFC 2018) blocks in our ACKs so the sender resends only what was lost, one
 * ACK per batch of received data; go-back-N retransmission from the first hole
 * with exponential backoff and a fast retransmit after three duplicate ACKs,
 * zero-window probing, MSS. Passive open (listen/accept) for local services. */

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define RTO_INITIAL 1000
#define RTO_MAX 30000
#define MAX_RETRIES 8
#define TIME_WAIT_MS 2000
#define PERSIST_MIN 250
#define PERSIST_MAX 5000
#define CHALLENGE_ACK_MS 100
#define DUPACK_THRESHOLD 3

/* Segment sizes. Over Ethernet: what fits a 1500-byte frame. Over loopback
 * every segment costs a queue entry, an ACK and a trip through the stack
 * whatever its size, so they are made large; 32 KiB still puts two segments
 * in the 64 KiB window (no window scaling), so the sender can refill one
 * half while the receiver takes the other. (16 KiB measured the same: the
 * copies, not the per-segment work, dominate at this size.) */
#define ETH_MSS (ETH_MTU - 40)
#define LO_MSS 32768

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t wnd, csum, urg;
} __attribute__((packed));

static inline bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint64_t now_ms(void) { return time_monotonic_ns() / 1000000ULL; }

static uint16_t route_mss(uint32_t dst) { return ip_is_local(dst) ? LO_MSS : ETH_MSS; }

/* The window to advertise, in the segment's own units (scaled unless it is a SYN, whose window never is). */
static uint16_t rx_window(struct sock *s, bool syn) {
    uint64_t used = s->rx_head - s->rx_tail;
    uint64_t space = s->rx_size > used ? s->rx_size - used : 0;
    if (!syn && s->rcv_wscale)
        space >>= s->rcv_wscale;
    return (uint16_t)(space > 65535 ? 65535 : space);
}

/* ---- segments that came ahead of a gap ---- */

struct tcp_ooo {
    struct tcp_ooo *next;
    uint32_t seq, len;
    bool fin;
    uint8_t data[];
};

#define OOO_MAX_SEGS 256

void tcp_drop_ooo(struct sock *s) {
    while (s->ooo) {
        struct tcp_ooo *o = s->ooo;
        s->ooo = o->next;
        kfree(o);
    }
    s->ooo_bytes = 0;
}

/* Keeps [seq, seq+len) for later, in order, without bytes it already has. */
static void ooo_insert(struct sock *s, uint32_t seq, const uint8_t *data, uint32_t len, bool fin) {
    int n = 0;
    for (struct tcp_ooo *o = s->ooo; o; o = o->next)
        n++;
    uint64_t space = s->rx_size - (s->rx_head - s->rx_tail);
    if (n >= OOO_MAX_SEGS || s->ooo_bytes + len > space) /* (beyond what the window offered: dropped, resent later) */
        return;
    struct tcp_ooo **pp = &s->ooo;
    while (*pp && seq_lt((*pp)->seq, seq))
        pp = &(*pp)->next;
    if (*pp && (*pp)->seq == seq && (*pp)->len >= len)
        return; /* a copy of one it has */
    struct tcp_ooo *o = kmalloc(sizeof *o + len);
    if (!o)
        return;
    o->seq = seq, o->len = len, o->fin = fin;
    memcpy(o->data, data, len);
    o->next = *pp;
    *pp = o;
    s->ooo_bytes += len;
}

/* The SACK blocks for an ACK: up to three ranges it holds beyond the gap, merged where they touch. */
static int sack_blocks(struct sock *s, uint32_t blocks[3][2]) {
    int n = 0;
    for (struct tcp_ooo *o = s->ooo; o; o = o->next) {
        if (n && seq_le(o->seq, blocks[n - 1][1])) {
            if (seq_lt(blocks[n - 1][1], o->seq + o->len))
                blocks[n - 1][1] = o->seq + o->len;
            continue;
        }
        if (n == 3)
            break;
        blocks[n][0] = o->seq, blocks[n][1] = o->seq + o->len;
        n++;
    }
    return n;
}

/* Copies `len` bytes of the send ring, starting `off` bytes after snd_una. */
void tcp_rx_append(struct sock *s, const uint8_t *data, size_t take);

static void tx_copy(struct sock *s, uint64_t off, uint8_t *dst, size_t len) {
    size_t at = (s->tx_off + off) % SOCK_TX_SIZE;
    size_t first = len < SOCK_TX_SIZE - at ? len : SOCK_TX_SIZE - at;
    memcpy(dst, s->tx + at, first);
    memcpy(dst + first, s->tx, len - first);
}

/* Sends one segment; its payload (if `len`) is the send ring's bytes from
 * `off` bytes after snd_una. */
static void send_segment(struct sock *s, uint8_t flags, uint32_t seq, uint64_t off, size_t len) {
    /* Static, not on the stack: a loopback segment is LO_MSS bytes. net_lock
     * (held) gives it one user at a time, and ip_send never calls back into
     * TCP (loopback delivery is deferred to net_unlock). */
    static uint8_t buf[sizeof(struct tcp_hdr) + 40 + LO_MSS];
    uint8_t *o = buf + sizeof(struct tcp_hdr);
    size_t olen = 0;
    if (flags & SYN) {
        /* MSS; window scale and SACK permitted: offered in a SYN, answered in a SYN-ACK only if the SYN offered */
        uint16_t mss = route_mss(s->rip);
        o[0] = 2, o[1] = 4, o[2] = (uint8_t)(mss >> 8), o[3] = (uint8_t)(mss & 0xff);
        olen = 4;
        bool offer = !(flags & ACK);
        if (offer || s->rcv_wscale)
            o[olen] = 1, o[olen + 1] = 3, o[olen + 2] = 3, o[olen + 3] = offer ? TCP_RCV_WSCALE : s->rcv_wscale, olen += 4;
        if (offer || s->sack_ok)
            o[olen] = 1, o[olen + 1] = 1, o[olen + 2] = 4, o[olen + 3] = 2, olen += 4;
    } else if ((flags & ACK) && s->sack_ok && s->ooo) {
        uint32_t b[3][2];
        int n = sack_blocks(s, b);
        o[0] = 1, o[1] = 1, o[2] = 5, o[3] = (uint8_t)(2 + 8 * n);
        for (int i = 0; i < n; i++) {
            uint32_t l = htonl(b[i][0]), r = htonl(b[i][1]);
            memcpy(o + 4 + 8 * i, &l, 4);
            memcpy(o + 8 + 8 * i, &r, 4);
        }
        olen = 4 + 8 * (size_t)n;
    }
    size_t hlen = sizeof(struct tcp_hdr) + olen;
    if (hlen + len > sizeof buf)
        return;
    struct tcp_hdr *h = (struct tcp_hdr *)buf;
    memset(h, 0, sizeof *h);
    h->sport = htons(s->lport);
    h->dport = htons(s->rport);
    h->seq = htonl(seq);
    h->ack = (flags & ACK) ? htonl(s->rcv_nxt) : 0;
    h->off = (uint8_t)((hlen / 4) << 4);
    h->flags = flags;
    h->wnd = htons(rx_window(s, flags & SYN));
    if (len)
        tx_copy(s, off, buf + hlen, len);
    uint32_t src = s->lip, dst = s->rip;
    size_t total = hlen + len;
    uint32_t ps = (src >> 16) + (src & 0xffff) + (dst >> 16) + (dst & 0xffff) + 6 + (uint32_t)total;
    h->csum = net_checksum(buf, total, ps);
    ip_send(src, dst, 6, buf, total);
}

void tcp_send_ack(struct sock *s) { send_segment(s, ACK, s->snd_nxt, 0, 0); }

#define DEFERRED 64
static struct sock *deferred[DEFERRED];
static int ndeferred;

void tcp_defer(struct sock *s) {
    for (int i = 0; i < ndeferred; i++)
        if (deferred[i] == s)
            return;
    if (ndeferred == DEFERRED) { /* (full: this one now) */
        waitq_wake_all(&s->wq);
        tcp_send_ack(s);
        return;
    }
    deferred[ndeferred++] = s;
}

int tcp_flush_deferred(void) {
    int n = ndeferred;
    for (int i = 0; i < ndeferred; i++) {
        struct sock *s = deferred[i];
        waitq_wake_all(&s->wq);
        if (s->state != TCP_CLOSED)
            tcp_send_ack(s);
    }
    ndeferred = 0;
    return n;
}

void tcp_forget_deferred(struct sock *s) {
    for (int i = 0; i < ndeferred; i++)
        if (deferred[i] == s)
            deferred[i--] = deferred[--ndeferred];
}

static void send_rst(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint32_t seq, uint32_t ack,
                     bool with_ack) {
    struct sock tmp = {0};
    tmp.lip = dst, tmp.lport = dport, tmp.rip = src, tmp.rport = sport, tmp.rcv_nxt = ack;
    send_segment(&tmp, (uint8_t)(RST | (with_ack ? ACK : 0)), seq, 0, 0);
}

static void arm_rto(struct sock *s) { s->rto_deadline = now_ms() + s->rto_ms; }

/* Sends whatever the window allows: new data, then a FIN once drained. */
void tcp_output(struct sock *s) {
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT && s->state != TCP_FIN_WAIT1 &&
        s->state != TCP_LAST_ACK && s->state != TCP_CLOSING)
        return;
    while (!s->fin_sent) {
        uint32_t inflight = s->snd_nxt - s->snd_una;
        uint64_t unsent = s->tx_len - inflight;
        if (!unsent || inflight >= s->snd_wnd)
            break;
        size_t n = unsent;
        if (n > s->mss)
            n = s->mss;
        if (n > s->snd_wnd - inflight)
            n = s->snd_wnd - inflight;
        send_segment(s, ACK | PSH, s->snd_nxt, inflight, n);
        s->snd_nxt += (uint32_t)n;
        if (!s->rto_deadline)
            arm_rto(s);
    }
    uint32_t inflight = s->snd_nxt - s->snd_una;
    if (s->fin_queued && !s->fin_sent && inflight == s->tx_len) {
        send_segment(s, FIN | ACK, s->snd_nxt, 0, 0);
        s->snd_nxt++;
        s->fin_sent = true;
        if (s->state == TCP_ESTABLISHED)
            s->state = TCP_FIN_WAIT1;
        else if (s->state == TCP_CLOSE_WAIT)
            s->state = TCP_LAST_ACK;
        if (!s->rto_deadline)
            arm_rto(s);
        inflight++;
    }
    if (!inflight)
        s->rto_deadline = 0; /* nothing to retransmit (go_back with a closed window) */
    /* Data waiting, nothing in flight and the peer's window closed: only the
     * peer's window update would get us going again, and if that is lost the
     * connection hangs. Probe instead (tcp_timers). */
    if (!s->snd_wnd && !inflight && s->tx_len) {
        if (!s->persist_deadline) {
            s->persist_ms = PERSIST_MIN;
            s->persist_deadline = now_ms() + s->persist_ms;
        }
    } else {
        s->persist_deadline = 0;
    }
}

/* Resends from the oldest unacknowledged byte. */
static void go_back(struct sock *s) {
    bool fin_was_sent = s->fin_sent && s->snd_nxt == s->snd_una + (uint32_t)s->tx_len + 1;
    s->snd_nxt = s->snd_una;
    s->dupacks = 0;
    if (fin_was_sent && s->tx_len == 0) {
        send_segment(s, FIN | ACK, s->snd_nxt, 0, 0);
        s->snd_nxt++;
    } else {
        s->fin_sent = false;
        tcp_output(s);
    }
}

int tcp_connect(struct sock *s) {
    /* A socket bound to a loopback address cannot reach the outside. */
    if (s->lip && (s->lip >> 24) == 127 && !ip_is_local(s->rip))
        return -ENETUNREACH;
    uint32_t src = s->lip ? s->lip : ip_source_for(s->rip);
    if (!src)
        return -ENETUNREACH;
    s->lip = src;
    if (!s->lport)
        s->lport = sock_ephemeral_port();
    if (!s->lport)
        return -EADDRNOTAVAIL;
    uint32_t r;
    void random_bytes(void *buf, size_t len);
    random_bytes(&r, sizeof r);
    s->iss = r;
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->mss = 536;
    s->rto_ms = RTO_INITIAL;
    s->retries = 0;
    s->error = 0;
    s->snd_wscale = s->rcv_wscale = 0, s->sack_ok = false;
    s->state = TCP_SYN_SENT;
    send_segment(s, SYN, s->iss, 0, 0);
    arm_rto(s);
    return 0;
}

int64_t tcp_send(struct sock *s, const uint8_t *data, size_t len) {
    if (s->error)
        return -s->error;
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT)
        return s->state == TCP_SYN_SENT ? -EAGAIN : -EPIPE;
    if (s->fin_queued)
        return -EPIPE;
    uint64_t space = SOCK_TX_SIZE - s->tx_len;
    size_t n = len < space ? len : space;
    if (!n)
        return -EAGAIN;
    size_t at = (s->tx_off + s->tx_len) % SOCK_TX_SIZE;
    size_t first = n < SOCK_TX_SIZE - at ? n : SOCK_TX_SIZE - at;
    memcpy(s->tx + at, data, first);
    memcpy(s->tx, data + first, n - first);
    s->tx_len += n;
    tcp_output(s);
    return (int64_t)n;
}

void tcp_close(struct sock *s) {
    switch (s->state) {
    case TCP_SYN_SENT:
    case TCP_LISTEN:
    case TCP_CLOSED:
        s->state = TCP_CLOSED;
        s->rto_deadline = 0;
        break;
    case TCP_SYN_RCVD:
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        s->fin_queued = true;
        tcp_output(s);
        break;
    default:
        break;
    }
}

static void set_closed(struct sock *s, int err) {
    s->state = TCP_CLOSED;
    if (err)
        s->error = err;
    s->rx_eof = true;
    s->rto_deadline = 0;
    s->persist_deadline = 0;
    waitq_wake_all(&s->wq);
}

/* A connection that was never accepted has no file to free it later. */
static void drop_unaccepted(struct sock *s) {
    if (s->state == TCP_SYN_RCVD) {
        s->listener = NULL;
        s->file_open = false;
    }
}

/* Returns whether `ack` acknowledged something new. */
static bool ack_received(struct sock *s, uint32_t ack) {
    if (!seq_lt(s->snd_una, ack) || seq_lt(s->snd_nxt, ack))
        return false;
    uint32_t acked = ack - s->snd_una;
    uint32_t data_acked = acked;
    if (s->fin_sent && ack == s->snd_nxt)
        data_acked--; /* the FIN's sequence number carries no data */
    if (data_acked > s->tx_len)
        data_acked = (uint32_t)s->tx_len;
    s->tx_off = (s->tx_off + data_acked) % SOCK_TX_SIZE;
    s->tx_len -= data_acked;
    s->snd_una = ack;
    s->retries = 0;
    s->rto_ms = RTO_INITIAL;
    s->rto_deadline = s->snd_una == s->snd_nxt ? 0 : now_ms() + s->rto_ms;
    waitq_wake_all(&s->wq);
    return true;
}

static void accept_child(struct sock *l, uint32_t src, uint16_t sport, uint32_t dst, const struct tcp_hdr *h,
                         uint16_t mss, int wscale, bool sack) {
    /* Half-open connections count against the backlog too: each one becomes
     * an accept-queue entry when its handshake completes. When full, the SYN
     * is dropped and the client's retransmission tries again later. */
    int pending = l->accept_count;
    for (struct sock *o = all_socks; o; o = o->next)
        if (o->listener == l && o->state == TCP_SYN_RCVD)
            pending++;
    if (pending >= l->backlog)
        return;
    struct sock *c = sock_alloc(1);
    if (!c)
        return;
    c->lip = dst, c->lport = l->lport, c->rip = src, c->rport = sport;
    c->rcv_nxt = ntohl(h->seq) + 1;
    uint32_t r;
    void random_bytes(void *buf, size_t len);
    random_bytes(&r, sizeof r);
    c->iss = r;
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = ntohs(h->wnd);
    c->mss = mss;
    if (wscale >= 0) /* both scale, or neither */
        c->snd_wscale = (uint8_t)wscale, c->rcv_wscale = TCP_RCV_WSCALE;
    c->sack_ok = sack;
    c->rto_ms = RTO_INITIAL;
    c->state = TCP_SYN_RCVD;
    c->listener = l;
    c->file_open = true; /* owned by the accept queue until accepted */
    send_segment(c, SYN | ACK, c->iss, 0, 0);
    arm_rto(c);
}

void tcp_listener_closed(struct sock *l) {
    for (struct sock *o = all_socks; o; o = o->next)
        if (o->listener == l)
            o->listener = NULL; /* half-open ones are refused when they complete */
}

/* A bigger ring, the bytes kept in place (positions are counted from the start, wrapped by the size). */
static void rx_grow(struct sock *s, uint32_t size) {
    uint8_t *n = kmalloc(size);
    if (!n)
        return;
    for (uint64_t i = s->rx_tail; i < s->rx_head; i++)
        n[i % size] = s->rx[i % s->rx_size];
    kfree(s->rx);
    s->rx = n;
    s->rx_size = size;
}

void tcp_rx_append(struct sock *s, const uint8_t *data, size_t take) {
    size_t at = s->rx_head % s->rx_size;
    size_t first = take < s->rx_size - at ? take : s->rx_size - at;
    memcpy(s->rx + at, data, first);
    memcpy(s->rx, data + first, take - first);
    s->rx_head += take;
    s->rcv_nxt += (uint32_t)take;
    /* A whole ring's worth in under a second: data is streaming, and the window may be what holds it back. */
    s->grow_bytes += take;
    if (s->grow_bytes >= s->rx_size) {
        uint64_t now = now_ms();
        if (s->rcv_wscale && s->rx_size < SOCK_RX_MAX && now - s->grow_ms < 1000)
            rx_grow(s, s->rx_size * 2);
        s->grow_bytes = 0;
        s->grow_ms = now;
    }
}

void tcp_input(struct netif *nif, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    (void)nif;
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *h = (const void *)seg;
    size_t hlen = (size_t)(h->off >> 4) * 4;
    if (hlen < 20 || hlen > len)
        return;
    uint16_t sport = ntohs(h->sport), dport = ntohs(h->dport);
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);
    uint8_t flags = h->flags;
    const uint8_t *data = seg + hlen;
    size_t dlen = len - hlen;
    uint16_t mss = 536;
    int wscale = -1; /* not offered */
    bool sack_perm = false;
    for (const uint8_t *o = seg + sizeof *h; o < seg + hlen;) {
        if (*o == 0)
            break;
        if (*o == 1) {
            o++;
            continue;
        }
        if (o + 1 >= seg + hlen || o[1] < 2 || o + o[1] > seg + hlen)
            break;
        if (o[0] == 2 && o[1] == 4)
            mss = (uint16_t)(o[2] << 8 | o[3]);
        else if (o[0] == 3 && o[1] == 3)
            wscale = o[2] > 14 ? 14 : o[2];
        else if (o[0] == 4 && o[1] == 2)
            sack_perm = true;
        o += o[1];
    }
    if (mss > route_mss(src))
        mss = route_mss(src);

    struct sock *s = sock_find(1, dst, dport, src, sport);
    if (!s) {
        if (!(flags & RST))
            send_rst(src, sport, dst, dport, (flags & ACK) ? ack : 0,
                     seq + (uint32_t)dlen + ((flags & SYN) ? 1 : 0) + ((flags & FIN) ? 1 : 0), !(flags & ACK));
        return;
    }

    if (s->state == TCP_LISTEN) {
        if (flags & RST)
            return;
        if (flags & ACK) /* for a connection that no longer exists */
            send_rst(src, sport, dst, dport, ack, 0, false);
        else if (flags & SYN)
            accept_child(s, src, sport, dst, h, mss, wscale, sack_perm);
        return;
    }

    if (flags & RST) {
        if (s->state == TCP_SYN_SENT && (!(flags & ACK) || ack != s->iss + 1))
            return; /* not an answer to our SYN */
        drop_unaccepted(s);
        set_closed(s, s->state == TCP_SYN_SENT ? ECONNREFUSED : ECONNRESET);
        sock_free_if_done(s);
        return;
    }

    if (s->state == TCP_SYN_SENT) {
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ack == s->iss + 1) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = ntohs(h->wnd); /* (a SYN's window is never scaled) */
            s->mss = mss;
            if (wscale >= 0) /* it answered our offer: both scale */
                s->snd_wscale = (uint8_t)wscale, s->rcv_wscale = TCP_RCV_WSCALE;
            s->sack_ok = sack_perm;
            s->state = TCP_ESTABLISHED;
            s->rto_deadline = 0;
            s->retries = 0;
            tcp_send_ack(s);
            tcp_output(s);
            waitq_wake_all(&s->wq);
        }
        return;
    }

    if (s->state == TCP_SYN_RCVD) {
        if (!(flags & ACK) || ack != s->iss + 1)
            return;
        s->snd_una = ack;
        s->state = TCP_ESTABLISHED;
        s->rto_deadline = 0;
        struct sock *l = s->listener;
        if (l && l->state == TCP_LISTEN && l->accept_count < SOCK_BACKLOG) {
            l->accept_q[l->accept_count++] = s;
            waitq_wake_all(&l->wq);
        } else {
            /* The listener closed during the handshake: refuse rather than
             * keep a connection nobody can accept. */
            send_segment(s, RST, s->snd_nxt, 0, 0);
            s->listener = NULL;
            s->file_open = false;
            set_closed(s, ECONNRESET);
            sock_free_if_done(s);
            return;
        }
    }

    if (flags & ACK) {
        bool advanced = ack_received(s, ack);
        s->snd_wnd = (uint32_t)ntohs(h->wnd) << s->snd_wscale;
        if (advanced) {
            s->dupacks = 0;
        } else if (ack == s->snd_una && !dlen && !(flags & (SYN | FIN)) && s->snd_nxt != s->snd_una &&
                   ++s->dupacks == DUPACK_THRESHOLD) {
            /* Acknowledgments that keep naming the same byte while more is in
             * flight: what follows it was lost, or dropped by a receiver that
             * ran out of room. Resend now rather than after a whole RTO. */
            go_back(s);
        }
    }

    bool need_ack = false;
    if ((dlen || (flags & FIN)) && seq_lt(seq, s->rcv_nxt)) { /* (partly) a copy of what it has: the rest is new */
        uint32_t old = s->rcv_nxt - seq;
        if (old >= dlen + ((flags & FIN) ? 1 : 0)) {
            dlen = 0, flags &= (uint8_t)~FIN;
            need_ack = true; /* all of it: a resend it didn't need; say where it is */
        } else {
            data += old < dlen ? old : dlen;
            dlen -= old < dlen ? old : dlen;
            seq = s->rcv_nxt;
        }
    }
    if (dlen || (flags & FIN)) {
        if (seq != s->rcv_nxt) {
            /* ahead of a gap: kept, and the ACK says so at once (its SACK blocks), so only the gap is resent */
            if (dlen)
                ooo_insert(s, seq, data, (uint32_t)dlen, flags & FIN);
            need_ack = true;
        } else {
            uint64_t space = s->rx_size - (s->rx_head - s->rx_tail);
            size_t take = dlen < space ? dlen : space;
            tcp_rx_append(s, data, take);
            /* what came ahead of the gap follows now, if the gap is filled */
            bool ooo_fin = false;
            while (s->ooo && seq_le(s->ooo->seq, s->rcv_nxt) && take == dlen) {
                struct tcp_ooo *o = s->ooo;
                s->ooo = o->next;
                s->ooo_bytes -= o->len;
                uint32_t skip = s->rcv_nxt - o->seq;
                if (skip < o->len) {
                    uint64_t room = s->rx_size - (s->rx_head - s->rx_tail);
                    uint32_t want = o->len - skip;
                    tcp_rx_append(s, o->data + skip, want < room ? want : room);
                    ooo_fin = o->fin && want <= room;
                }
                kfree(o);
                need_ack = true; /* (the gap closed: the sender is told at once) */
            }
            if (ooo_fin)
                flags |= FIN, take = dlen; /* the FIN that came ahead of the gap */
            if (!need_ack)
                need_ack = (flags & FIN) || take < dlen; /* a FIN, or no room: told now; plain data: at the batch's end */
            if (!need_ack && take)
                tcp_defer(s);
            if ((flags & FIN) && take == dlen) {
                s->rcv_nxt++;
                s->rx_eof = true;
                if (s->state == TCP_ESTABLISHED)
                    s->state = TCP_CLOSE_WAIT;
                else if (s->state == TCP_FIN_WAIT1)
                    s->state = s->snd_una == s->snd_nxt ? TCP_TIME_WAIT : TCP_CLOSING;
                else if (s->state == TCP_FIN_WAIT2)
                    s->state = TCP_TIME_WAIT;
                if (s->state == TCP_TIME_WAIT)
                    s->timewait_deadline = now_ms() + TIME_WAIT_MS;
            }
            if (flags & FIN)
                waitq_wake_all(&s->wq);
        }
    } else if (seq_lt(seq, s->rcv_nxt) && now_ms() - s->challenge_ack_ms >= CHALLENGE_ACK_MS) {
        /* An empty segment from before our window: a zero-window probe (see
         * tcp_timers) or a peer that lost track. Our ACK tells it where we
         * are and how much room we have. Rate-limited, so two confused ends
         * cannot keep answering each other. */
        s->challenge_ack_ms = now_ms();
        need_ack = true;
    }

    /* Our FIN got acknowledged. */
    if (s->fin_sent && s->snd_una == s->snd_nxt) {
        if (s->state == TCP_FIN_WAIT1)
            s->state = TCP_FIN_WAIT2;
        else if (s->state == TCP_CLOSING) {
            s->state = TCP_TIME_WAIT;
            s->timewait_deadline = now_ms() + TIME_WAIT_MS;
        } else if (s->state == TCP_LAST_ACK) {
            set_closed(s, 0);
            sock_free_if_done(s);
            return;
        }
    }
    if (need_ack)
        tcp_send_ack(s);
    tcp_output(s);
}

void tcp_timers(uint64_t now) {
    /* Safe to walk while sending: loopback replies are only delivered at
     * net_unlock, so nothing here can free a socket other than `s`. */
    for (struct sock *s = all_socks, *next; s; s = next) {
        next = s->next;
        if (s->type != 1)
            continue;
        if (s->state == TCP_TIME_WAIT && now >= s->timewait_deadline) {
            set_closed(s, 0);
            sock_free_if_done(s);
            continue;
        }
        if (s->persist_deadline && now >= s->persist_deadline) {
            /* Zero-window probe: an empty segment with an old sequence number,
             * which the peer answers with an ACK carrying its window. Not a
             * retry: a peer may keep its window shut as long as its reader
             * is busy, and that is no reason to drop the connection. */
            send_segment(s, ACK, s->snd_una - 1, 0, 0);
            s->persist_ms = s->persist_ms * 2 > PERSIST_MAX ? PERSIST_MAX : s->persist_ms * 2;
            s->persist_deadline = now + s->persist_ms;
        }
        if (!s->rto_deadline || now < s->rto_deadline)
            continue;
        if (++s->retries > MAX_RETRIES) {
            drop_unaccepted(s);
            set_closed(s, ETIMEDOUT);
            sock_free_if_done(s);
            continue;
        }
        s->rto_ms = s->rto_ms * 2 > RTO_MAX ? RTO_MAX : s->rto_ms * 2;
        arm_rto(s);
        if (s->state == TCP_SYN_SENT)
            send_segment(s, SYN, s->iss, 0, 0);
        else if (s->state == TCP_SYN_RCVD)
            send_segment(s, SYN | ACK, s->iss, 0, 0);
        else
            go_back(s);
    }
}
