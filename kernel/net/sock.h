#pragma once
#include <kernel/net.h>
#include <kernel/proc.h>

/* Internal socket state shared by socket.c, udp.c and tcp.c. */

#define SOCK_RX_SIZE (128 * 1024) /* a power of two: the receive ring wraps with % */
#define SOCK_TX_SIZE (128 * 1024) /* a power of two too */
#define UDP_QUEUE 64
#define SOCK_BACKLOG 64 /* connections waiting for accept(), half-open ones included */

enum tcp_state {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
};

struct dgram {
    uint32_t src;
    uint16_t sport;
    uint16_t len;
    uint8_t *data;
};

struct sock {
    int type; /* 1 = stream, 2 = dgram */
    /* lip is 0 (any address) until bind() names one or a connection picks
     * the source address for its route; received packets must match it. */
    uint32_t lip, rip;
    uint16_t lport, rport;
    int error;      /* pending error (positive errno) */
    bool shut_rd, shut_wr;
    bool file_open; /* still referenced by a file */
    bool reuseaddr, nodelay, keepalive; /* accepted and reported back; see setsockopt */
    struct waitq wq;

    /* stream receive ring (in-order bytes) */
    uint8_t *rx;
    uint64_t rx_head, rx_tail; /* written / read, ever */
    bool rx_eof;

    /* udp */
    struct dgram q[UDP_QUEUE];
    int q_head, q_count;

    /* tcp */
    enum tcp_state state;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint16_t mss;
    uint8_t *tx;               /* ring of the bytes from snd_una onward... */
    uint64_t tx_off;           /* ...which start here (ACKs advance it, nothing moves) */
    uint64_t tx_len;           /* bytes buffered (unacked + unsent) */
    bool fin_queued, fin_sent;
    uint64_t rto_ms, rto_deadline, timewait_deadline;
    int retries;
    int dupacks;                            /* ACKs in a row that acknowledged nothing new */
    uint64_t persist_ms, persist_deadline;  /* probing a closed peer window */
    uint64_t challenge_ack_ms;              /* last ACK sent for an out-of-window segment */

    /* listening */
    struct sock *accept_q[SOCK_BACKLOG];
    int accept_count, backlog;
    struct sock *listener; /* for a connection not yet accepted */

    struct sock *next;
};

extern struct sock *all_socks;

struct sock *sock_alloc(int type);
void sock_free_if_done(struct sock *s);
uint16_t sock_ephemeral_port(void);
struct sock *sock_find(int type, uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport);

/* AF_UNIX (unix.c). The socket system calls hand over AF_UNIX descriptors. */
struct linux_iovec;
bool unix_is_socket(struct file *f);
int64_t unix_socket(uint64_t type, uint64_t protocol);
int64_t unix_socketpair(uint64_t type, uint64_t protocol, uint64_t usv);
int64_t unix_bind(struct file *f, uint64_t uaddr, uint64_t len);
int64_t unix_listen(struct file *f, int backlog);
int64_t unix_connect(struct file *f, uint64_t uaddr, uint64_t len);
int64_t unix_accept(struct file *f, uint64_t uaddr, uint64_t ulen, uint64_t flags);
int64_t unix_sendmsg(struct file *f, const struct linux_iovec *iov, uint64_t iovcnt, uint64_t uname, uint32_t namelen,
                     uint64_t ucontrol, uint64_t controllen, uint64_t flags);
int64_t unix_recvmsg(struct file *f, const struct linux_iovec *iov, uint64_t iovcnt, uint64_t uname, uint32_t *namelen,
                     uint64_t ucontrol, uint64_t *controllen, int *msg_flags, uint64_t flags);
int64_t unix_shutdown(struct file *f, uint64_t how);
int64_t unix_getname(struct file *f, uint64_t uaddr, uint64_t ulen, bool peer);
int64_t unix_getsockopt(struct file *f, uint64_t level, uint64_t opt, uint64_t uval, uint64_t ulen);
int64_t unix_setsockopt(struct file *f, uint64_t level, uint64_t opt, uint64_t uval, uint64_t len);

int udp_sendto(struct sock *s, uint32_t dst, uint16_t dport, const void *data, size_t len);
int tcp_connect(struct sock *s);
int64_t tcp_send(struct sock *s, const uint8_t *data, size_t len);
void tcp_close(struct sock *s);
void tcp_listener_closed(struct sock *l);
void tcp_output(struct sock *s);
void tcp_send_ack(struct sock *s);
