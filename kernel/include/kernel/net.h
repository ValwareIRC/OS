#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/spinlock.h>

/* IPv4 networking: Ethernet, ARP, IPv4, ICMP echo, UDP, TCP, a DHCP client
 * and loopback. One network thread drives the NICs: it polls the drivers,
 * processes frames, and runs protocol timers. Loopback packets are delivered
 * by whoever releases net_lock (net_unlock). All protocol state is guarded
 * by net_lock; socket calls block on per-socket wait queues. */

#define ETH_MTU 1500
#define ETH_FRAME_MAX 1518
/* Loopback carries IP datagrams of any legal size: nothing is fragmented and
 * a whole 64 KiB UDP datagram or a large TCP segment moves as one packet. */
#define LO_MTU 65535
#define IP_LOOPBACK 0x7f000001u /* 127.0.0.1 */

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

/* Addresses are kept in host byte order inside the stack. */
#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

struct netif {
    char name[8];
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    bool configured;
    /* Driver hooks. tx may be called with net_lock held; must not sleep. */
    int (*tx)(struct netif *nif, const void *frame, size_t len);
    /* Delivers received frames by calling net_rx; returns frames handled. */
    int (*poll)(struct netif *nif);
    /* Is a cable in (carrier)? NULL: assume so. */
    bool (*link)(struct netif *nif);
    void *drv;
    uint64_t rx_packets, tx_packets;
};

extern spinlock_t net_lock;

/* Releases net_lock taken with spin_lock_irqsave(&net_lock, ...). Packets
 * sent to this machine are queued on "lo" while the lock is held and are
 * delivered here, before the lock goes, so the stack never re-enters itself. */
void net_unlock(uint64_t flags);

void net_init(void);
void net_register(struct netif *nif);
struct netif *net_default(void);
/* An interface as netlink describes it (netinfo.c): 1 is lo, 2 the adapter. */
struct net_ifinfo {
    char name[8];
    int index;
    bool loopback, up, running;
    uint8_t mac[6];
    uint32_t ip, mask, gw;
};
bool net_if_get(int index, struct net_ifinfo *out);
/* AF_NETLINK, NETLINK_ROUTE (netlink.c) */
struct file;
int64_t netlink_socket(uint64_t type, uint64_t proto);
bool netlink_is_socket(struct file *f);
int64_t netlink_bind(struct file *f, uint64_t uaddr, uint64_t len);
int64_t netlink_getname(struct file *f, uint64_t uaddr, uint64_t ulen);
int64_t netlink_send(struct file *f, const void *buf, size_t len);
int64_t netlink_recv(struct file *f, uint64_t ubuf, size_t len, uint64_t flags, uint64_t uname, uint64_t ulen,
                     int *msg_flags);
bool net_is_loopback(const struct netif *nif);
void net_rx(struct netif *nif, const uint8_t *frame, size_t len);
uint16_t net_checksum(const void *data, size_t len, uint32_t initial);

/* Interface information for programs (netinfo.c): the SIOCGIF* socket
 * ioctls, and /proc/net/dev and /proc/net/route. */
struct snode;
struct sbuf;
int64_t net_if_ioctl(uint64_t req, uint64_t arg);
int procnet_dev(struct snode *n, struct sbuf *b);
int procnet_route(struct snode *n, struct sbuf *b);

/* True for 127.0.0.0/8 and the addresses of configured interfaces. */
bool ip_is_local(uint32_t addr);
/* The source address for packets to `dst`, or 0 when there is no route. */
uint32_t ip_source_for(uint32_t dst);
/* The largest IP datagram (header included) the route to `dst` carries. */
size_t ip_mtu_for(uint32_t dst);

/* Sends an IPv4 packet (payload after the IP header) from `src`. Local
 * destinations go through the loopback queue. net_lock held. */
int ip_send(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len);

/* Protocol entry points (net_lock held). */
void udp_input(struct netif *nif, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_input(struct netif *nif, uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_timers(uint64_t now_ms);
void dhcp_input(struct netif *nif, const uint8_t *msg, size_t len);
void dhcp_tick(struct netif *nif, uint64_t now_ms);
void dhcp_start(struct netif *nif);

/* Drivers */
void e1000_init(void);

/* Socket syscalls live in net/socket.c. */
