#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/usb.h>

/* xHCI (eXtensible Host Controller Interface, Intel's specification of 2010, rev 1.2): the one USB controller of
 * every computer made since, for every speed (USB 1, 2, 3). The controller is told what to do through rings of
 * 16-byte "transfer request blocks" in memory and a doorbell per device; it answers on an event ring. Here:
 *   - the controller taken from the firmware (which drives the keyboard itself until then), reset, started;
 *   - its ports watched: what is plugged in is reset, given an address, read (its descriptors), configured,
 *     and offered to the class drivers (hid.c, msc.c); hubs are handled here, their ports watched the same way;
 *   - control, bulk and interrupt transfers for the class drivers.
 * No interrupts: one thread per controller looks at the event ring every few milliseconds (a keyboard is read
 * every 8 at most), and a transfer someone waits for works the ring itself until its end. */

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_RESET_EP 14
#define TRB_STOP_EP 15
#define TRB_SET_TR_DEQ 16
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND 33
#define TRB_EV_PORT 34

#define TRB_CYCLE (1u << 0)
#define TRB_TC (1u << 1) /* link: toggle cycle */
#define TRB_ISP (1u << 2)
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)

#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT 13

/* operational registers */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_PAGESIZE 0x08
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))
#define CMD_RUN (1u << 0)
#define CMD_RESET (1u << 1)
#define STS_HALTED (1u << 0)
#define STS_NOT_READY (1u << 11)

/* a port's status and control */
#define PORT_CCS (1u << 0)
#define PORT_PED (1u << 1)
#define PORT_PR (1u << 4)
#define PORT_PP (1u << 9)
#define PORT_SPEED(v) (((v) >> 10) & 0xf)
#define PORT_CHANGES (0x7fu << 17)
#define PORT_PRC (1u << 21)
#define PORT_WPR (1u << 31)
/* the bits that must be written back as they are (the rest are cleared by writing 1, or start something) */
#define PORT_RO ((1u << 0) | (1u << 3) | (0xfu << 10) | (1u << 30))
#define PORT_RWS ((0xfu << 5) | (1u << 9) | (0x3u << 14) | (0x7u << 25))

#define RING_TRBS 256
#define EVENT_TRBS 256
#define POLL_MS 4
#define BOUNCE 65536

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *trb;
    uint64_t phys;
    int enq;
    uint32_t cycle;
    uint8_t dci;
    /* a transfer waited for */
    uint64_t wait_trb, data_trb;
    volatile bool done;
    uint32_t cc, residual, data_residual;
    /* an endpoint read over and over (usb_poll_endpoint) */
    bool polled, needs_reset;
    uint16_t poll_len;
    uint8_t *poll_buf;
    uint64_t poll_phys;
    void (*poll_done)(struct usb_device *d, void *arg, const uint8_t *data, int len);
    void *poll_arg;
};

struct xhci {
    struct pci_dev *pci;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    int max_slots, max_ports, ctx_size;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *evt;
    uint64_t evt_phys;
    int evt_deq;
    uint32_t evt_cycle;
    uint64_t cmd_wait;
    volatile bool cmd_done;
    uint32_t cmd_cc;
    int cmd_slot;
    struct usb_device *slots[256];
    uint8_t port_major[256]; /* 2 or 3, from the Supported Protocol capabilities */
    volatile int lock;
    struct usb_device *devices;
    bool ports_changed;
    int index;
    struct xhci *next;
};

/* what a device needs besides struct usb_device */
struct xdev {
    uint64_t in_phys;
    uint8_t *in;
    uint8_t *ctl_buf; /* control transfers' data (a page) */
    uint64_t ctl_phys;
    uint8_t *bounce; /* bulk transfers' data, 64 KiB, never across a 64 KiB line */
    uint64_t bounce_phys;
};

static struct xhci *controllers;
static struct usb_driver *drivers;
static int ncontrollers;

static void enumerate_children(struct xhci *h, struct usb_device *hub);
static void disconnect(struct xhci *h, struct usb_device *d);

/* ---- small things ---- */

static void wr64(volatile uint8_t *p, uint64_t v) { /* low half first: some controllers take 64-bit writes so only */
    mmio_write32(p, (uint32_t)v);
    mmio_write32(p + 4, (uint32_t)(v >> 32));
}

static void *dma_page(uint64_t *phys) {
    uint64_t p = pmm_alloc(1);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, PAGE_SIZE);
    *phys = p;
    return v;
}

static void hc_lock(struct xhci *h) {
    while (__atomic_exchange_n(&h->lock, 1, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
}

static void hc_unlock(struct xhci *h) { __atomic_store_n(&h->lock, 0, __ATOMIC_RELEASE); }

static bool wait_reg(volatile uint8_t *reg, uint32_t mask, uint32_t want, int ms) {
    for (int i = 0; i < ms * 10; i++) {
        if ((mmio_read32(reg) & mask) == want)
            return true;
        if (i % 10 == 9)
            thread_sleep_ms(1);
    }
    return (mmio_read32(reg) & mask) == want;
}

static bool ring_init(struct ring *r) {
    memset(r, 0, sizeof *r);
    r->trb = dma_page(&r->phys);
    r->cycle = 1;
    return r->trb != NULL;
}

/* One TRB onto a ring (the cycle bit, which hands it to the controller, written last); returns its address. */
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control) {
    struct trb *t = &r->trb[r->enq];
    uint64_t at = r->phys + (uint64_t)r->enq * sizeof *t;
    t->param = param;
    t->status = status;
    dma_wmb();
    t->control = (control & ~TRB_CYCLE) | r->cycle;
    if (++r->enq == RING_TRBS - 1) { /* the last is a link back to the start, which flips the cycle */
        struct trb *l = &r->trb[RING_TRBS - 1];
        l->param = r->phys;
        l->status = 0;
        dma_wmb();
        l->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CHAIN) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void *ctx(struct xhci *h, uint8_t *base, int index) { return base + (size_t)index * (size_t)h->ctx_size; }

/* ---- the event ring ---- */

static void transfer_event(struct xhci *h, struct usb_device *d, struct ring *r, uint64_t trb, uint32_t cc,
                           uint32_t residual) {
    (void)h;
    if (r->polled && trb == r->wait_trb) {
        if (cc == CC_SUCCESS || cc == CC_SHORT) {
            if (!d->gone && r->poll_done)
                r->poll_done(d, r->poll_arg, r->poll_buf, (int)r->poll_len - (int)residual);
            if (!d->gone) { /* the next one */
                r->wait_trb = ring_push(r, r->poll_phys, r->poll_len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
                mmio_write32(&h->db[d->slot], r->dci);
            }
        } else if (!d->gone) {
            r->needs_reset = true; /* (the thread resets it: a command can't be waited for in here) */
        }
        return;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT) { /* an error anywhere in it (a stall in its setup or data) ends it */
        r->cc = cc;
        r->residual = 0;
        r->done = true;
        return;
    }
    if (trb == r->data_trb)
        r->data_residual = residual;
    if (trb == r->wait_trb) {
        r->cc = cc;
        r->residual = residual;
        r->done = true;
    }
}

static void process_events(struct xhci *h) {
    bool any = false;
    for (;;) {
        struct trb *e = &h->evt[h->evt_deq];
        uint32_t c = __atomic_load_n(&e->control, __ATOMIC_ACQUIRE);
        if ((c & TRB_CYCLE) != h->evt_cycle)
            break;
        dma_rmb();
        uint32_t type = (c >> 10) & 0x3f, cc = e->status >> 24;
        if (type == TRB_EV_COMMAND) {
            if (e->param == h->cmd_wait) {
                h->cmd_cc = cc;
                h->cmd_slot = (int)(c >> 24);
                h->cmd_done = true;
            }
        } else if (type == TRB_EV_TRANSFER) {
            int slot = (int)(c >> 24), dci = (int)((c >> 16) & 0x1f);
            struct usb_device *d = slot > 0 && slot <= h->max_slots ? h->slots[slot] : NULL;
            struct ring *r = d ? d->rings[dci] : NULL;
            if (r)
                transfer_event(h, d, r, e->param, cc, e->status & 0xffffff);
        } else if (type == TRB_EV_PORT) {
            h->ports_changed = true;
        }
        if (++h->evt_deq == EVENT_TRBS) {
            h->evt_deq = 0;
            h->evt_cycle ^= 1;
        }
        any = true;
    }
    if (any) /* where it has been read to (and the "busy" flag cleared) */
        wr64(h->rt + 0x20 + 0x18, (h->evt_phys + (uint64_t)h->evt_deq * sizeof(struct trb)) | (1u << 3));
}

/* A command, waited for: its completion code (and the slot it names, for Enable Slot). */
static uint32_t command(struct xhci *h, uint64_t param, uint32_t status, uint32_t control, int *slot) {
    h->cmd_done = false;
    h->cmd_wait = ring_push(&h->cmd, param, status, control);
    mmio_write32(&h->db[0], 0);
    for (int i = 0; i < 3000 && !h->cmd_done; i++) {
        process_events(h);
        if (!h->cmd_done)
            thread_sleep_ms(1);
    }
    if (!h->cmd_done) {
        kprintf("xhci%d: command %u had no answer\n", h->index, (control >> 10) & 0x3f);
        return 0;
    }
    if (slot)
        *slot = h->cmd_slot;
    return h->cmd_cc;
}

/* A transfer's end, waited for (timeout_ms 0: as long as it takes, while the device is there). */
static int wait_ring(struct xhci *h, struct usb_device *d, struct ring *r, int timeout_ms) {
    uint64_t start = time_monotonic_ns();
    while (!r->done) {
        process_events(h);
        if (r->done)
            break;
        if (d->gone)
            return -ENODEV;
        if (timeout_ms && time_monotonic_ns() - start > (uint64_t)timeout_ms * 1000000ull)
            return -ETIMEDOUT;
        thread_sleep_ms(1);
    }
    return 0;
}

/* After a stall or a transfer given up: the endpoint reset (a stalled one) or stopped, and its ring started
 * again past what it was doing. */
static void ring_recover(struct xhci *h, struct usb_device *d, struct ring *r, bool stalled) {
    uint32_t sc = (uint32_t)d->slot << 24 | (uint32_t)r->dci << 16;
    if (stalled)
        command(h, 0, 0, TRB_TYPE(TRB_RESET_EP) | sc, NULL);
    else
        command(h, 0, 0, TRB_TYPE(TRB_STOP_EP) | sc, NULL);
    uint64_t deq = r->phys + (uint64_t)r->enq * sizeof(struct trb);
    command(h, deq | r->cycle, 0, TRB_TYPE(TRB_SET_TR_DEQ) | sc, NULL);
}

/* ---- transfers ---- */

int usb_control(struct usb_device *d, uint8_t reqtype, uint8_t req, uint16_t value, uint16_t index, void *data,
                uint16_t len) {
    struct xhci *h = d->hc;
    struct xdev *x = d->hcd;
    if (d->gone)
        return -ENODEV;
    if (len > PAGE_SIZE)
        return -EINVAL;
    hc_lock(h);
    struct ring *r = d->rings[1];
    bool in = reqtype & USB_DIR_IN;
    if (!in && len)
        memcpy(x->ctl_buf, data, len);
    uint64_t setup = (uint64_t)reqtype | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 |
                     (uint64_t)len << 48;
    uint32_t trt = !len ? 0 : in ? 3 : 2;
    r->done = false;
    r->data_trb = 0;
    r->data_residual = 0;
    ring_push(r, setup, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16);
    if (len)
        r->data_trb = ring_push(r, x->ctl_phys, len, TRB_TYPE(TRB_DATA) | (in ? TRB_DIR_IN : 0) | TRB_IOC);
    r->wait_trb = ring_push(r, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (in && len ? 0 : TRB_DIR_IN));
    mmio_write32(&h->db[d->slot], 1);
    int err = wait_ring(h, d, r, 2000);
    int got = 0;
    if (!err && r->cc != CC_SUCCESS && r->cc != CC_SHORT)
        err = r->cc == CC_STALL ? -EPIPE : -EIO;
    if (err) {
        if (err != -ENODEV)
            ring_recover(h, d, r, err == -EPIPE);
    } else {
        got = len - (int)r->data_residual;
        if (in && got > 0)
            memcpy(data, x->ctl_buf, (size_t)got);
    }
    hc_unlock(h);
    return err ? err : got;
}

static int dci_of(uint8_t ep_addr) { return (ep_addr & 0xf) * 2 + ((ep_addr & USB_DIR_IN) ? 1 : 0); }

/* Configure Endpoint with only the slot context: what the slot context says (a hub's ports and translator,
 * the last endpoint in use) taken by the controller. */
static uint32_t configure(struct xhci *h, struct usb_device *d, uint32_t add) {
    struct xdev *x = d->hcd;
    uint32_t *icc = (uint32_t *)x->in;
    icc[0] = 0;
    icc[1] = add | 1; /* (the slot always) */
    return command(h, x->in_phys, 0, TRB_TYPE(TRB_CONFIGURE_EP) | (uint32_t)d->slot << 24, NULL);
}

/* the input context's slot context, from what the controller holds now */
static uint32_t *slot_from_output(struct xhci *h, struct usb_device *d) {
    struct xdev *x = d->hcd;
    memset(x->in, 0, PAGE_SIZE);
    uint32_t *s = ctx(h, x->in, 1);
    memcpy(s, ctx(h, d->out_ctx, 0), (size_t)h->ctx_size);
    s[3] = 0; /* (state and address: the controller's) */
    return s;
}

static int interval_of(struct usb_device *d, const struct usb_endpoint *ep) {
    int iv = ep->interval ? ep->interval : 1;
    if ((ep->attributes & 3) != 3)
        return 0;
    if (d->speed == USB_SPEED_HIGH || d->speed >= USB_SPEED_SUPER) /* 2^(bInterval-1) microframes */
        return iv > 16 ? 15 : iv - 1;
    int n = 3; /* full and low speed: bInterval in milliseconds, as a power of two of 125 µs */
    while (n < 10 && (1 << (n + 1)) <= iv * 8)
        n++;
    return n;
}

int usb_enable_endpoint(struct usb_device *d, const struct usb_endpoint *ep) {
    struct xhci *h = d->hc;
    int dci = dci_of(ep->address);
    if (d->gone)
        return -ENODEV;
    hc_lock(h);
    struct ring *r = d->rings[dci];
    if (!r) {
        r = kzalloc(sizeof *r);
        if (!r || !ring_init(r)) {
            kfree(r);
            hc_unlock(h);
            return -ENOMEM;
        }
        r->dci = (uint8_t)dci;
        d->rings[dci] = r;
    }
    uint32_t *s = slot_from_output(h, d);
    uint32_t entries = s[0] >> 27;
    if ((uint32_t)dci > entries)
        s[0] = (s[0] & 0x07ffffffu) | (uint32_t)dci << 27;
    struct xdev *x = d->hcd;
    uint32_t *e = ctx(h, x->in, dci + 1);
    bool in = ep->address & USB_DIR_IN;
    int kind = ep->attributes & 3; /* 2 bulk, 3 interrupt */
    uint32_t type = kind == 2 ? (in ? 6 : 2) : kind == 3 ? (in ? 7 : 3) : (in ? 5 : 1);
    uint32_t mps = ep->max_packet & 0x7ff, burst = ep->max_burst;
    if (d->speed == USB_SPEED_HIGH && kind == 3)
        burst = (ep->max_packet >> 11) & 3;
    uint32_t esit = kind == 3 ? mps * (burst + 1) : 0;
    e[0] = (uint32_t)interval_of(d, ep) << 16;
    e[1] = 3u << 1 | type << 3 | burst << 8 | mps << 16;
    e[2] = (uint32_t)r->phys | 1;
    e[3] = (uint32_t)(r->phys >> 32);
    e[4] = (kind == 3 ? mps : 1024) | (esit & 0xffff) << 16;
    uint32_t cc = configure(h, d, 1u << dci);
    hc_unlock(h);
    if (cc != CC_SUCCESS) {
        kprintf("usb: %s: endpoint %02x couldn't be readied (%u)\n", d->name, ep->address, cc);
        return -EIO;
    }
    return 0;
}

int usb_transfer(struct usb_device *d, uint8_t ep_addr, void *data, uint32_t len, int timeout_ms) {
    struct xhci *h = d->hc;
    struct xdev *x = d->hcd;
    struct ring *r = d->rings[dci_of(ep_addr)];
    bool in = ep_addr & USB_DIR_IN;
    if (!r)
        return -EINVAL;
    uint32_t done = 0;
    while (done < len || (!len && !done)) {
        if (d->gone)
            return -ENODEV;
        uint32_t n = len - done > BOUNCE ? BOUNCE : len - done;
        hc_lock(h);
        if (!in && n)
            memcpy(x->bounce, (uint8_t *)data + done, n);
        r->done = false;
        r->data_trb = 0;
        r->wait_trb = ring_push(r, x->bounce_phys, n, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
        mmio_write32(&h->db[d->slot], r->dci);
        int err = wait_ring(h, d, r, timeout_ms);
        if (!err && r->cc != CC_SUCCESS && r->cc != CC_SHORT)
            err = r->cc == CC_STALL ? -EPIPE : -EIO;
        if (err) {
            if (err != -ENODEV)
                ring_recover(h, d, r, err == -EPIPE);
            hc_unlock(h);
            return err;
        }
        uint32_t got = n - (r->residual > n ? n : r->residual);
        if (in && got)
            memcpy((uint8_t *)data + done, x->bounce, got);
        hc_unlock(h);
        done += got;
        if (!len || got < n) /* a short packet ends it */
            break;
    }
    return (int)done;
}

int usb_clear_halt(struct usb_device *d, uint8_t ep_addr) {
    int r = usb_control(d, USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */, ep_addr, NULL, 0);
    struct ring *ring = d->rings[dci_of(ep_addr)];
    if (ring && !d->gone) {
        hc_lock(d->hc);
        ring_recover(d->hc, d, ring, true);
        hc_unlock(d->hc);
    }
    return r < 0 ? r : 0;
}

int usb_poll_endpoint(struct usb_device *d, uint8_t ep_addr, uint16_t len,
                      void (*done)(struct usb_device *d, void *arg, const uint8_t *data, int len), void *arg) {
    struct xhci *h = d->hc;
    struct ring *r = d->rings[dci_of(ep_addr)];
    if (!r || len > PAGE_SIZE)
        return -EINVAL;
    hc_lock(h);
    if (!r->poll_buf)
        r->poll_buf = dma_page(&r->poll_phys);
    if (!r->poll_buf) {
        hc_unlock(h);
        return -ENOMEM;
    }
    r->poll_len = len;
    r->poll_done = done;
    r->poll_arg = arg;
    r->polled = true;
    r->wait_trb = ring_push(r, r->poll_phys, len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    mmio_write32(&h->db[d->slot], r->dci);
    hc_unlock(h);
    return 0;
}

void usb_register_driver(struct usb_driver *drv) {
    drv->next = drivers;
    drivers = drv;
}

/* ---- a device arrives ---- */

static void read_string(struct usb_device *d, uint8_t index, char *out, size_t n) {
    out[0] = 0;
    uint8_t buf[256];
    if (!index || usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8 | index, 0x0409, buf,
                              sizeof buf) < 2)
        return;
    size_t k = 0;
    for (int i = 2; i + 1 < buf[0] && k + 1 < n; i += 2) /* UTF-16: what is ASCII, kept */
        out[k++] = buf[i + 1] || buf[i] < 32 || buf[i] > 126 ? '?' : (char)buf[i];
    out[k] = 0;
    while (k && out[k - 1] == ' ')
        out[--k] = 0;
}

static void parse_config(struct usb_device *d) {
    const uint8_t *p = d->config, *end = d->config + d->config_len;
    struct usb_interface *cur = NULL;
    d->ninterfaces = 0;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DT_INTERFACE && p[0] >= 9) {
            cur = NULL;
            if (p[3] == 0 && d->ninterfaces < 8) { /* alternate settings other than 0 aren't used */
                cur = &d->intf[d->ninterfaces++];
                memset(cur, 0, sizeof *cur);
                cur->number = p[2], cur->alt = p[3], cur->cls = p[5], cur->subclass = p[6], cur->protocol = p[7];
                cur->extra = p + p[0];
            }
        } else if (p[1] == USB_DT_ENDPOINT && p[0] >= 7 && cur && cur->nendpoints < 8) {
            struct usb_endpoint *e = &cur->ep[cur->nendpoints++];
            e->address = p[2], e->attributes = p[3], e->max_packet = (uint16_t)(p[4] | p[5] << 8), e->interval = p[6];
            if (p + p[0] + 6 <= end && p[p[0] + 1] == 0x30) /* SuperSpeed endpoint companion */
                e->max_burst = p[p[0] + 2];
        } else if (cur && cur->nendpoints == 0) {
            cur->extra_len = (int)(p + p[0] - cur->extra); /* (class descriptors before its endpoints: HID's) */
        }
        p += p[0];
    }
}

/* The translator a full or low speed device behind a high speed hub is reached through. */
static void tt_of(struct usb_device *parent, int port, int *tt_slot, int *tt_port, bool *multi) {
    *tt_slot = *tt_port = 0;
    *multi = false;
    for (; parent; port = parent->parent_port, parent = parent->parent)
        if (parent->speed == USB_SPEED_HIGH) {
            *tt_slot = parent->slot;
            *tt_port = port;
            *multi = parent->hub_tt_multi;
            return;
        }
}

static struct usb_device *enumerate(struct xhci *h, struct usb_device *parent, int parent_port, int root_port,
                                    uint32_t route, enum usb_speed speed) {
    int slot = 0;
    if (command(h, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot) != CC_SUCCESS || slot <= 0 || slot > h->max_slots) {
        kprintf("xhci%d: no slot for a new device\n", h->index);
        return NULL;
    }
    struct usb_device *d = kzalloc(sizeof *d);
    struct xdev *x = kzalloc(sizeof *x);
    struct ring *r0 = kzalloc(sizeof *r0);
    uint64_t bp = pmm_alloc(32);
    if (!d || !x || !r0 || !bp || !ring_init(r0)) {
        kfree(d), kfree(x), kfree(r0);
        command(h, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
        return NULL;
    }
    d->hc = h, d->slot = slot, d->speed = speed, d->root_port = root_port, d->route = route;
    d->parent = parent, d->parent_port = parent_port, d->depth = parent ? parent->depth + 1 : 0;
    d->hcd = x;
    r0->dci = 1;
    d->rings[1] = r0;
    d->out_ctx = dma_page(&d->out_ctx_phys);
    x->in = dma_page(&x->in_phys);
    x->ctl_buf = dma_page(&x->ctl_phys);
    x->bounce_phys = (bp + BOUNCE - 1) & ~(uint64_t)(BOUNCE - 1);
    x->bounce = phys_to_virt(x->bounce_phys);
    if (!d->out_ctx || !x->in || !x->ctl_buf)
        goto fail;
    h->dcbaa[slot] = d->out_ctx_phys;
    h->slots[slot] = d;
    ksnprintf(d->name, sizeof d->name, "usb%d-%d", h->index, root_port);

    /* Address Device: the slot and endpoint 0 */
    uint32_t *icc = (uint32_t *)x->in;
    icc[1] = 3; /* slot, endpoint 0 */
    uint32_t *s = ctx(h, x->in, 1);
    s[0] = (route & 0xfffff) | (uint32_t)speed << 20 | 1u << 27;
    s[1] = (uint32_t)root_port << 16;
    int tt_slot, tt_port;
    bool multi;
    tt_of(parent, parent_port, &tt_slot, &tt_port, &multi);
    if ((speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) && tt_slot) {
        s[2] = (uint32_t)tt_slot | (uint32_t)tt_port << 8;
        if (multi)
            s[0] |= 1u << 25;
    }
    uint32_t mps0 = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    uint32_t *e0 = ctx(h, x->in, 2);
    e0[1] = 3u << 1 | 4u << 3 | mps0 << 16;
    e0[2] = (uint32_t)r0->phys | 1;
    e0[3] = (uint32_t)(r0->phys >> 32);
    e0[4] = 8;
    uint32_t cc = command(h, x->in_phys, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | (uint32_t)slot << 24, NULL);
    if (cc != CC_SUCCESS) {
        kprintf("usb: %s: it wasn't given an address (%u)\n", d->name, cc);
        goto fail;
    }
    hc_unlock(h); /* (the transfers below take it themselves) */
    thread_sleep_ms(2);
    uint8_t dd[18];
    int got = usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, dd, 8);
    if (got < 8) {
        hc_lock(h);
        kprintf("usb: %s: it doesn't answer (%d)\n", d->name, got);
        goto fail;
    }
    uint32_t mps = speed >= USB_SPEED_SUPER ? 1u << (dd[7] & 15) : dd[7];
    if (mps != mps0 && mps >= 8) { /* endpoint 0 is larger than first thought: Evaluate Context */
        hc_lock(h);
        memset(x->in, 0, PAGE_SIZE);
        icc[1] = 2;
        e0 = ctx(h, x->in, 2);
        memcpy(e0, ctx(h, d->out_ctx, 1), (size_t)h->ctx_size);
        e0[1] = (e0[1] & 0xffff) | mps << 16;
        command(h, x->in_phys, 0, TRB_TYPE(TRB_EVALUATE_CTX) | (uint32_t)slot << 24, NULL);
        hc_unlock(h);
    }
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, dd, 18) < 18) {
        hc_lock(h);
        goto fail;
    }
    d->vendor = (uint16_t)(dd[8] | dd[9] << 8), d->product = (uint16_t)(dd[10] | dd[11] << 8);
    d->cls = dd[4], d->subclass = dd[5], d->protocol = dd[6];
    uint8_t c9[9];
    if (usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, c9, 9) < 9) {
        hc_lock(h);
        goto fail;
    }
    int total = c9[2] | c9[3] << 8;
    if (total > (int)PAGE_SIZE)
        total = (int)PAGE_SIZE;
    d->config = kmalloc((size_t)total);
    if (!d->config || (d->config_len = usb_control(d, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
                                                   d->config, (uint16_t)total)) < 9) {
        hc_lock(h);
        goto fail;
    }
    parse_config(d);
    char product[48], maker[32];
    read_string(d, dd[15], product, sizeof product);
    read_string(d, dd[14], maker, sizeof maker);
    if (product[0])
        ksnprintf(d->name, sizeof d->name, "%s%s%s", maker, maker[0] ? " " : "", product);
    else
        ksnprintf(d->name, sizeof d->name, "USB device %04x:%04x", d->vendor, d->product);
    if (usb_control(d, USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, d->config[5], 0, NULL, 0) < 0) {
        hc_lock(h);
        kprintf("usb: %s: its configuration wasn't taken\n", d->name);
        goto fail;
    }
    static const char *const sp[] = {"?", "full speed", "low speed", "high speed", "5 Gb/s", "10 Gb/s"};
    kprintf("usb: %s (%04x:%04x, %s, port %d%s)\n", d->name, d->vendor, d->product, sp[speed <= 5 ? speed : 0],
            root_port, parent ? " behind a hub" : "");
    d->is_hub = d->cls == USB_CLASS_HUB || (d->ninterfaces && d->intf[0].cls == USB_CLASS_HUB);
    if (!d->is_hub)
        for (int i = 0; i < d->ninterfaces; i++)
            for (struct usb_driver *drv = drivers; drv; drv = drv->next)
                if (drv->probe(d, i)) {
                    d->drv[i] = drv;
                    break;
                }
    hc_lock(h);
    d->next = h->devices;
    h->devices = d;
    if (d->is_hub)
        enumerate_children(h, d); /* (its ports read now, and watched from then on) */
    return d;
fail:
    h->slots[slot] = NULL;
    h->dcbaa[slot] = 0;
    command(h, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)slot << 24, NULL);
    kfree(d->config);
    kfree(d);
    return NULL;
}

static struct usb_device *child_at(struct xhci *h, struct usb_device *parent, int port) {
    for (struct usb_device *d = h->devices; d; d = d->next)
        if (d->parent == parent && (parent ? d->parent_port == port : d->root_port == port))
            return d;
    return NULL;
}

static void disconnect(struct xhci *h, struct usb_device *d) {
    for (bool more = true; more;) { /* what is behind it (a hub's) first */
        more = false;
        for (struct usb_device *k = h->devices; k; k = k->next)
            if (k->parent == d && !k->gone) {
                disconnect(h, k);
                more = true;
                break;
            }
    }
    d->gone = true;
    kprintf("usb: %s was unplugged\n", d->name);
    hc_unlock(h);
    for (int i = 0; i < d->ninterfaces; i++)
        if (d->drv[i] && d->drv[i]->disconnect)
            d->drv[i]->disconnect(d, i);
    hc_lock(h);
    command(h, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)d->slot << 24, NULL);
    h->slots[d->slot] = NULL;
    h->dcbaa[d->slot] = 0;
    for (struct usb_device **pp = &h->devices; *pp; pp = &(*pp)->next)
        if (*pp == d) {
            *pp = d->next;
            break;
        }
    /* (its memory is kept: a class driver's thread may still hold the device a moment longer) */
}

/* ---- hubs ---- */

#define HUB_REQ_IN (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER)
#define HUB_REQ_OUT (USB_TYPE_CLASS | USB_RECIP_OTHER)
#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_RESET 20

static int hub_status(struct usb_device *hub, int port, uint16_t *status, uint16_t *change) {
    uint8_t b[4];
    int r = usb_control(hub, HUB_REQ_IN, USB_REQ_GET_STATUS, 0, (uint16_t)port, b, 4);
    if (r < 4)
        return r < 0 ? r : -EIO;
    *status = (uint16_t)(b[0] | b[1] << 8);
    *change = (uint16_t)(b[2] | b[3] << 8);
    return 0;
}

static bool hub_setup(struct xhci *h, struct usb_device *hub) {
    bool ss = hub->speed >= USB_SPEED_SUPER;
    uint8_t desc[16];
    hc_unlock(h);
    int r = usb_control(hub, USB_DIR_IN | USB_TYPE_CLASS, USB_REQ_GET_DESCRIPTOR,
                        (ss ? USB_DT_SS_HUB : USB_DT_HUB) << 8, 0, desc, sizeof desc);
    if (r < 7) {
        hc_lock(h);
        return false;
    }
    hub->hub_ports = desc[2] > 15 ? 15 : desc[2];
    hub->hub_tt_multi = hub->protocol == 2;
    if (ss) /* its depth, for the route it puts in its packets */
        usb_control(hub, USB_TYPE_CLASS | USB_RECIP_DEVICE, 12 /* SET_HUB_DEPTH */, (uint16_t)hub->depth, 0, NULL, 0);
    hc_lock(h);
    uint32_t *s = slot_from_output(h, hub);
    s[0] |= 1u << 26 | (hub->hub_tt_multi ? 1u << 25 : 0);
    s[1] = (s[1] & 0x00ffffffu) | (uint32_t)hub->hub_ports << 24;
    if (!ss)
        s[2] = (s[2] & ~(3u << 16)) | (uint32_t)((desc[3] >> 5) & 3) << 16;
    configure(h, hub, 0);
    hc_unlock(h);
    for (int p = 1; p <= hub->hub_ports; p++)
        usb_control(hub, HUB_REQ_OUT, USB_REQ_SET_FEATURE, HUB_PORT_POWER, (uint16_t)p, NULL, 0);
    thread_sleep_ms(desc[5] * 2 + 20);
    hc_lock(h);
    kprintf("usb: %s is a hub with %d ports\n", hub->name, hub->hub_ports);
    return true;
}

static void hub_port(struct xhci *h, struct usb_device *hub, int port) {
    uint16_t st, ch;
    hc_unlock(h);
    int r = hub_status(hub, port, &st, &ch);
    if (!r && (ch & 1))
        usb_control(hub, HUB_REQ_OUT, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, (uint16_t)port, NULL, 0);
    hc_lock(h);
    if (r)
        return;
    struct usb_device *child = child_at(h, hub, port);
    bool connected = st & 1;
    if (child && (!connected || (ch & 1)))
        disconnect(h, child), child = NULL;
    if (!connected || child)
        return;
    hc_unlock(h);
    usb_control(hub, HUB_REQ_OUT, USB_REQ_SET_FEATURE, HUB_PORT_RESET, (uint16_t)port, NULL, 0);
    bool reset = false;
    for (int i = 0; i < 50 && !reset; i++) {
        thread_sleep_ms(10);
        reset = !hub_status(hub, port, &st, &ch) && (ch & (1u << 4));
    }
    usb_control(hub, HUB_REQ_OUT, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, (uint16_t)port, NULL, 0);
    thread_sleep_ms(10);
    hc_lock(h);
    if (!reset || !(st & 2)) {
        kprintf("usb: %s port %d: what is plugged in didn't come up after a reset\n", hub->name, port);
        return;
    }
    enum usb_speed sp = hub->speed >= USB_SPEED_SUPER ? USB_SPEED_SUPER
                        : (st & (1u << 9))           ? USB_SPEED_LOW
                        : (st & (1u << 10))          ? USB_SPEED_HIGH
                                                     : USB_SPEED_FULL;
    uint32_t route = hub->route | (uint32_t)port << (4 * hub->depth);
    enumerate(h, hub, port, hub->root_port, route, sp);
}

static void enumerate_children(struct xhci *h, struct usb_device *hub) {
    if (!hub->hub_ports && !hub_setup(h, hub))
        return;
    for (int p = 1; p <= hub->hub_ports && !hub->gone; p++)
        hub_port(h, hub, p);
}

/* ---- the controller's own ports ---- */

static void root_port(struct xhci *h, int port) {
    volatile uint8_t *reg = h->op + OP_PORTSC(port);
    uint32_t v = mmio_read32(reg);
    uint32_t neutral = (v & PORT_RO) | (v & PORT_RWS);
    if (v & PORT_CHANGES)
        mmio_write32(reg, neutral | (v & PORT_CHANGES));
    struct usb_device *dev = child_at(h, NULL, port);
    bool connected = v & PORT_CCS;
    if (dev && (!connected || (v & (1u << 17) /* connection changed: something else now */)))
        disconnect(h, dev), dev = NULL;
    if (!connected || dev)
        return;
    bool usb3 = h->port_major[port] == 3;
    if (!usb3) { /* USB 2 ports are reset to be enabled; USB 3 ones enable themselves */
        mmio_write32(reg, neutral | PORT_PR);
        if (!wait_reg(reg, PORT_PRC, PORT_PRC, 500)) {
            kprintf("xhci%d: port %d didn't finish its reset\n", h->index, port);
            return;
        }
        v = mmio_read32(reg);
        mmio_write32(reg, ((v & PORT_RO) | (v & PORT_RWS)) | (v & PORT_CHANGES));
    } else if (!wait_reg(reg, PORT_PED, PORT_PED, 300)) {
        v = mmio_read32(reg); /* a warm reset, for a link that came up badly */
        mmio_write32(reg, ((v & PORT_RO) | (v & PORT_RWS)) | PORT_WPR);
        wait_reg(reg, PORT_PRC, PORT_PRC, 500);
        v = mmio_read32(reg);
        mmio_write32(reg, ((v & PORT_RO) | (v & PORT_RWS)) | (v & PORT_CHANGES));
    }
    v = mmio_read32(reg);
    if (!(v & PORT_PED)) {
        kprintf("xhci%d: port %d has something that didn't come up\n", h->index, port);
        return;
    }
    hc_unlock(h);
    thread_sleep_ms(10); /* (reset recovery) */
    hc_lock(h);
    enumerate(h, NULL, 0, port, 0, (enum usb_speed)PORT_SPEED(v));
}

static void hc_thread(void *arg) {
    struct xhci *h = arg;
    uint64_t last_scan = 0, last_hubs = 0;
    hc_lock(h);
    for (int p = 1; p <= h->max_ports; p++)
        root_port(h, p);
    hc_unlock(h);
    for (;;) {
        hc_lock(h);
        process_events(h);
        uint64_t now = time_monotonic_ns() / 1000000ull;
        if (h->ports_changed || now - last_scan > 1000) { /* (a missed change caught within a second) */
            h->ports_changed = false;
            last_scan = now;
            for (int p = 1; p <= h->max_ports; p++)
                root_port(h, p);
        }
        if (now - last_hubs > 250) { /* hubs' ports, asked (each hub answers in a few microseconds) */
            last_hubs = now;
            for (struct usb_device *d = h->devices; d; d = d->next)
                if (d->is_hub && d->hub_ports && !d->gone)
                    for (int p = 1; p <= d->hub_ports && !d->gone; p++)
                        hub_port(h, d, p);
        }
        for (struct usb_device *d = h->devices; d; d = d->next) /* polled endpoints that stopped on an error */
            for (int i = 2; i < 32; i++) {
                struct ring *r = d->rings[i];
                if (r && r->polled && r->needs_reset && !d->gone) {
                    r->needs_reset = false;
                    ring_recover(h, d, r, true);
                    r->wait_trb = ring_push(r, r->poll_phys, r->poll_len, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
                    mmio_write32(&h->db[d->slot], r->dci);
                }
            }
        hc_unlock(h);
        thread_sleep_ms(POLL_MS);
    }
}

/* ---- the controller ---- */

static void take_from_firmware(struct xhci *h, uint32_t hcc1) {
    uint32_t off = (hcc1 >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        volatile uint8_t *p = h->cap + off;
        uint32_t v = mmio_read32(p);
        uint8_t id = v & 0xff, next = (v >> 8) & 0xff;
        if (id == 1) { /* USB Legacy Support: the firmware lets go of the controller */
            mmio_write32(p, v | (1u << 24));
            if (!wait_reg(p, 1u << 16, 0, 1000))
                kprintf("xhci%d: the firmware didn't let go of it; taken anyway\n", h->index);
            uint32_t ctl = mmio_read32(p + 4);
            ctl &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15)); /* its SMIs off */
            mmio_write32(p + 4, ctl | (7u << 29));
        } else if (id == 2) { /* Supported Protocol: which ports are USB 2 and which USB 3 */
            uint32_t major = v >> 24, ports = mmio_read32(p + 8);
            int first = ports & 0xff, count = (ports >> 8) & 0xff;
            for (int i = first; i < first + count && i < 256; i++)
                h->port_major[i] = (uint8_t)major;
        }
        off = next ? off + ((uint32_t)next << 2) : 0;
    }
}

static void bring_up(struct pci_dev *pd) {
    if (pd->bar_is_io[0] || !pd->bar[0])
        return;
    struct xhci *h = kzalloc(sizeof *h);
    if (!h)
        return;
    h->pci = pd;
    h->index = ncontrollers;
    pci_enable(pd);
    vmm_map_mmio(pd->bar[0], pd->bar_size[0] ? pd->bar_size[0] : 0x10000, true);
    h->cap = phys_to_virt(pd->bar[0]);
    uint8_t caplen = mmio_read8(h->cap);
    uint32_t hcs1 = mmio_read32(h->cap + 4), hcs2 = mmio_read32(h->cap + 8), hcc1 = mmio_read32(h->cap + 0x10);
    h->op = h->cap + caplen;
    h->db = (volatile uint32_t *)(h->cap + (mmio_read32(h->cap + 0x14) & ~3u));
    h->rt = h->cap + (mmio_read32(h->cap + 0x18) & ~0x1fu);
    h->max_slots = hcs1 & 0xff;
    h->max_ports = (int)(hcs1 >> 24);
    h->ctx_size = (hcc1 & 4) ? 64 : 32;
    if (h->max_slots > 255)
        h->max_slots = 255;
    if (!(hcc1 & 1))
        kprintf("xhci%d: 32-bit addresses only (memory above 4 GiB may not reach it)\n", ncontrollers);
    take_from_firmware(h, hcc1);
    /* stopped, then reset */
    mmio_write32(h->op + OP_USBCMD, mmio_read32(h->op + OP_USBCMD) & ~CMD_RUN);
    wait_reg(h->op + OP_USBSTS, STS_HALTED, STS_HALTED, 100);
    mmio_write32(h->op + OP_USBCMD, CMD_RESET);
    thread_sleep_ms(2); /* (some of Intel's must not be touched for a moment after) */
    if (!wait_reg(h->op + OP_USBCMD, CMD_RESET, 0, 1000) || !wait_reg(h->op + OP_USBSTS, STS_NOT_READY, 0, 1000)) {
        kprintf("xhci%d: it didn't come out of its reset\n", ncontrollers);
        return;
    }
    if (!(mmio_read32(h->op + OP_PAGESIZE) & 1)) {
        kprintf("xhci%d: it doesn't use 4 KiB pages; left alone\n", ncontrollers);
        return;
    }
    mmio_write32(h->op + OP_CONFIG, (uint32_t)h->max_slots);
    uint64_t dp;
    h->dcbaa = dma_page(&dp);
    if (!h->dcbaa || !ring_init(&h->cmd))
        return;
    int scratch = (int)(((hcs2 >> 21) & 0x1f) << 5 | ((hcs2 >> 27) & 0x1f));
    if (scratch) { /* memory it keeps for itself */
        uint64_t ap;
        uint64_t *arr = dma_page(&ap);
        if (!arr)
            return;
        for (int i = 0; i < scratch && i < 512; i++) {
            uint64_t sp;
            if (!dma_page(&sp))
                return;
            arr[i] = sp;
        }
        h->dcbaa[0] = ap;
    }
    wr64(h->op + OP_DCBAAP, dp);
    wr64(h->op + OP_CRCR, h->cmd.phys | 1);
    /* the event ring: one segment */
    uint64_t ep_;
    h->evt = dma_page(&h->evt_phys);
    uint64_t *erst = dma_page(&ep_);
    if (!h->evt || !erst)
        return;
    erst[0] = h->evt_phys;
    erst[1] = EVENT_TRBS;
    h->evt_cycle = 1;
    volatile uint8_t *ir0 = h->rt + 0x20;
    mmio_write32(ir0 + 0x08, 1);
    wr64(ir0 + 0x18, h->evt_phys);
    wr64(ir0 + 0x10, ep_);
    mmio_write32(h->op + OP_USBCMD, CMD_RUN);
    if (!wait_reg(h->op + OP_USBSTS, STS_HALTED, 0, 100)) {
        kprintf("xhci%d: it didn't start\n", ncontrollers);
        return;
    }
    for (int p = 1; p <= h->max_ports; p++) { /* every port powered */
        volatile uint8_t *reg = h->op + OP_PORTSC(p);
        uint32_t v = mmio_read32(reg);
        if (!(v & PORT_PP))
            mmio_write32(reg, (v & PORT_RO) | (v & PORT_RWS) | PORT_PP);
    }
    kprintf("xhci%d: %02x:%02x.%u: %d ports, %d slots\n", ncontrollers, pd->bus, pd->dev, pd->fn, h->max_ports,
            h->max_slots);
    ncontrollers++;
    h->next = controllers;
    controllers = h;
    char name[16];
    ksnprintf(name, sizeof name, "xhci%d", h->index);
    KASSERT(thread_create(name, hc_thread, h));
}

void usb_hid_init(void);
void usb_msc_init(void);

void usb_init(void) {
    usb_hid_init();
    usb_msc_init();
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->class_code == 0x0c && d->subclass == 0x03 && d->prog_if == 0x30)
            bring_up(d);
    }
}
