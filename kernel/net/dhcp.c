#include <kernel/lib.h>
#include <kernel/net.h>

/* Minimal DHCP client (RFC 2131): DISCOVER -> OFFER -> REQUEST -> ACK,
 * retried every two seconds until configured. Leases are not renewed yet;
 * QEMU's and home routers' leases outlast a demo session by far. */

#define DHCP_CLIENT 68
#define DHCP_SERVER 67

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

enum { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND };

static struct {
    int state;
    uint32_t xid, offered, server;
    uint64_t last_ms;
} st;

int udp_send_raw(uint32_t src_ip, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);

static void send_msg(struct netif *nif, uint8_t type) {
    struct dhcp_msg m;
    memset(&m, 0, sizeof m);
    m.op = 1;
    m.htype = 1;
    m.hlen = 6;
    m.xid = htonl(st.xid);
    m.flags = htons(0x8000); /* ask for broadcast replies: we have no IP yet */
    memcpy(m.chaddr, nif->mac, 6);
    m.magic = htonl(0x63825363);
    uint8_t *o = m.options;
    *o++ = 53, *o++ = 1, *o++ = type;
    if (type == 3) {
        *o++ = 50, *o++ = 4;
        uint32_t ip = htonl(st.offered);
        memcpy(o, &ip, 4), o += 4;
        *o++ = 54, *o++ = 4;
        uint32_t sv = htonl(st.server);
        memcpy(o, &sv, 4), o += 4;
    }
    *o++ = 55, *o++ = 3, *o++ = 1, *o++ = 3, *o++ = 6; /* want: mask, router, dns */
    *o++ = 12, *o++ = 5, memcpy(o, "os", 5), o += 5;
    *o++ = 255;
    udp_send_raw(0, DHCP_CLIENT, 0xffffffff, DHCP_SERVER, &m, sizeof m);
}

void dhcp_start(struct netif *nif) {
    st.xid = (uint32_t)(nif->mac[2] << 24 | nif->mac[3] << 16 | nif->mac[4] << 8 | nif->mac[5]) ^ 0x4c554d45;
    st.state = DHCP_SELECTING;
    st.last_ms = 0;
}

void dhcp_tick(struct netif *nif, uint64_t now) {
    if (st.state == DHCP_BOUND || st.state == DHCP_IDLE)
        return;
    if (st.last_ms && now - st.last_ms < 2000)
        return;
    st.last_ms = now;
    if (st.state == DHCP_REQUESTING)
        send_msg(nif, 3);
    else
        send_msg(nif, 1);
}

void dhcp_input(struct netif *nif, const uint8_t *data, size_t len) {
    if (len < 240)
        return;
    const struct dhcp_msg *m = (const void *)data;
    if (m->op != 2 || ntohl(m->xid) != st.xid || ntohl(m->magic) != 0x63825363)
        return;
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0;
    const uint8_t *o = m->options, *end = data + len;
    while (o < end && *o != 255) {
        if (*o == 0) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        uint8_t code = o[0], olen = o[1];
        const uint8_t *v = o + 2;
        uint32_t word = olen >= 4 ? (uint32_t)(v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]) : 0;
        if (code == 53 && olen >= 1)
            type = v[0];
        else if (code == 1)
            mask = word;
        else if (code == 3)
            router = word;
        else if (code == 6)
            dns = word;
        else if (code == 54)
            server = word;
        o += 2 + olen;
    }
    if (type == 2 && st.state == DHCP_SELECTING) { /* OFFER */
        st.offered = ntohl(m->yiaddr);
        st.server = server;
        st.state = DHCP_REQUESTING;
        st.last_ms = 0;
        dhcp_tick(nif, 1); /* send REQUEST now */
    } else if (type == 5 && st.state == DHCP_REQUESTING) { /* ACK */
        nif->ip = ntohl(m->yiaddr);
        nif->mask = mask ? mask : 0xffffff00;
        nif->gw = router;
        nif->dns = dns ? dns : router;
        nif->configured = true;
        st.state = DHCP_BOUND;
    } else if (type == 6) { /* NAK: start over */
        st.state = DHCP_SELECTING;
        st.last_ms = 0;
    }
}
