#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/net.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* Realtek's wired network chips, the most common on PC boards and laptops: the RTL8111/8168 (gigabit) and the
 * RTL8125 (2.5 gigabit). Both have rings of 16-byte descriptors the chip reads and writes by DMA; the 8125
 * moved its interrupt registers and its transmit doorbell. The PHY (the part on the cable) is left as the
 * firmware set it up and negotiates by itself; the MAC is reset and given its rings. Polled by the network
 * thread (net.c), as a card without interrupts is. */

#define REG_MAC0 0x00
#define REG_MAR0 0x08
#define REG_TXDESC_LO 0x20
#define REG_TXDESC_HI 0x24
#define REG_CMD 0x37
#define REG_TXPOLL_8168 0x38
#define REG_IMR_8168 0x3c /* 16-bit */
#define REG_ISR_8168 0x3e
#define REG_IMR_8125 0x38 /* 32-bit */
#define REG_ISR_8125 0x3c
#define REG_TXCFG 0x40
#define REG_RXCFG 0x44
#define REG_9346 0x50
#define REG_PHYSTATUS 0x6c
#define REG_TXPOLL_8125 0x90
#define REG_RXMAXSIZE 0xda
#define REG_CPLUS 0xe0
#define REG_RXDESC_LO 0xe4
#define REG_RXDESC_HI 0xe8
#define REG_MAXTX 0xec

#define CMD_RESET 0x10
#define CMD_RX 0x08
#define CMD_TX 0x04
#define TXPOLL_NPQ 0x40

#define DESC_OWN (1u << 31)
#define DESC_EOR (1u << 30)
#define DESC_FS (1u << 29)
#define DESC_LS (1u << 28)
#define RX_RES (1u << 21) /* an error in the frame */

#define NRX 64
#define NTX 64
#define BUF 2048

struct desc {
    volatile uint32_t opts1, opts2;
    volatile uint64_t addr;
};

struct rtl {
    struct netif nif;
    volatile uint8_t *mmio;
    bool is8125;
    struct desc *rx, *tx;
    uint8_t *rx_buf[NRX], *tx_buf[NTX];
    int rx_cur, tx_cur;
};

static void *dma(size_t bytes, uint64_t *phys) {
    size_t pages = ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE;
    uint64_t p = pmm_alloc(pages);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    *phys = p;
    return v;
}

static int rtl_tx(struct netif *nif, const void *frame, size_t len) {
    struct rtl *r = nif->drv;
    if (len > BUF - 4)
        return -1;
    struct desc *d = &r->tx[r->tx_cur];
    if (d->opts1 & DESC_OWN)
        return -1; /* ring full */
    memcpy(r->tx_buf[r->tx_cur], frame, len);
    if (len < 60) { /* the chip pads short frames only on some versions */
        memset(r->tx_buf[r->tx_cur] + len, 0, 60 - len);
        len = 60;
    }
    d->opts2 = 0;
    dma_wmb();
    d->opts1 = DESC_OWN | DESC_FS | DESC_LS | (r->tx_cur == NTX - 1 ? DESC_EOR : 0) | (uint32_t)len;
    r->tx_cur = (r->tx_cur + 1) % NTX;
    dma_wmb();
    if (r->is8125)
        mmio_write16(r->mmio + REG_TXPOLL_8125, 1);
    else
        mmio_write8(r->mmio + REG_TXPOLL_8168, TXPOLL_NPQ);
    nif->tx_packets++;
    return 0;
}

static bool rtl_link(struct netif *nif) {
    struct rtl *r = nif->drv;
    return mmio_read8(r->mmio + REG_PHYSTATUS) & 0x02;
}

static int rtl_poll(struct netif *nif) {
    struct rtl *r = nif->drv;
    int n = 0;
    for (; n < NRX; n++) {
        struct desc *d = &r->rx[r->rx_cur];
        uint32_t o = d->opts1;
        if (o & DESC_OWN)
            break;
        dma_rmb();
        uint32_t len = o & 0x3fff;
        if (!(o & RX_RES) && (o & DESC_FS) && (o & DESC_LS) && len > 4)
            net_rx(nif, r->rx_buf[r->rx_cur], len - 4); /* (without the CRC) */
        nif->rx_packets++;
        d->opts2 = 0;
        dma_wmb();
        d->opts1 = DESC_OWN | (r->rx_cur == NRX - 1 ? DESC_EOR : 0) | BUF; /* back to the chip */
        r->rx_cur = (r->rx_cur + 1) % NRX;
    }
    if (n) /* (what it said, read and cleared: nothing waits on it, but a full status register stalls some) */
        r->is8125 ? mmio_write32(r->mmio + REG_ISR_8125, mmio_read32(r->mmio + REG_ISR_8125))
                  : mmio_write16(r->mmio + REG_ISR_8168, mmio_read16(r->mmio + REG_ISR_8168));
    return n;
}

static void bring_up(struct pci_dev *pd) {
    int bar = -1; /* the registers: the first memory BAR (BAR 2 on most, BAR 1 on some) */
    for (int i = 0; i < 6 && bar < 0; i++)
        if (pd->bar[i] && !pd->bar_is_io[i] && pd->bar_size[i] >= 256)
            bar = i;
    if (bar < 0)
        return;
    struct rtl *r = kzalloc(sizeof *r);
    if (!r)
        return;
    r->is8125 = pd->device == 0x8125 || pd->device == 0x3000;
    pci_enable(pd);
    vmm_map_mmio(pd->bar[bar], pd->bar_size[bar], true);
    r->mmio = phys_to_virt(pd->bar[bar]);
    /* interrupts off, the MAC reset */
    if (r->is8125) {
        mmio_write32(r->mmio + REG_IMR_8125, 0);
        mmio_write32(r->mmio + REG_ISR_8125, 0xffffffffu);
    } else {
        mmio_write16(r->mmio + REG_IMR_8168, 0);
        mmio_write16(r->mmio + REG_ISR_8168, 0xffff);
    }
    mmio_write8(r->mmio + REG_CMD, CMD_RESET);
    for (int i = 0; i < 1000 && (mmio_read8(r->mmio + REG_CMD) & CMD_RESET); i++)
        thread_sleep_ms(1);
    if (mmio_read8(r->mmio + REG_CMD) & CMD_RESET) {
        kprintf("r8169: %04x didn't come out of its reset\n", pd->device);
        return;
    }
    for (int i = 0; i < 6; i++) /* its address, as the firmware loaded it */
        r->nif.mac[i] = mmio_read8(r->mmio + REG_MAC0 + i);
    uint64_t rx_phys, tx_phys;
    r->rx = dma(sizeof(struct desc) * NRX, &rx_phys);
    r->tx = dma(sizeof(struct desc) * NTX, &tx_phys);
    if (!r->rx || !r->tx)
        return;
    for (int i = 0; i < NRX; i++) {
        uint64_t p;
        if (!(r->rx_buf[i] = dma(BUF, &p)))
            return;
        r->rx[i].addr = p;
        r->rx[i].opts1 = DESC_OWN | (i == NRX - 1 ? DESC_EOR : 0) | BUF;
    }
    for (int i = 0; i < NTX; i++) {
        uint64_t p;
        if (!(r->tx_buf[i] = dma(BUF, &p)))
            return;
        r->tx[i].addr = p;
        r->tx[i].opts1 = i == NTX - 1 ? DESC_EOR : 0;
    }
    dma_wmb();
    mmio_write8(r->mmio + REG_9346, 0xc0); /* the configuration registers unlocked */
    mmio_write16(r->mmio + REG_CPLUS, (uint16_t)(mmio_read16(r->mmio + REG_CPLUS) & ~(1u << 5 /* RX VLAN */)));
    mmio_write16(r->mmio + REG_RXMAXSIZE, BUF);
    if (!r->is8125)
        mmio_write8(r->mmio + REG_MAXTX, 0x3f);
    mmio_write32(r->mmio + REG_TXDESC_LO, (uint32_t)tx_phys);
    mmio_write32(r->mmio + REG_TXDESC_HI, (uint32_t)(tx_phys >> 32));
    mmio_write32(r->mmio + REG_RXDESC_LO, (uint32_t)rx_phys);
    mmio_write32(r->mmio + REG_RXDESC_HI, (uint32_t)(rx_phys >> 32));
    mmio_write8(r->mmio + REG_CMD, CMD_RX | CMD_TX);
    /* DMA bursts unlimited; frames to it, broadcast and multicast taken */
    uint32_t rxcfg = 7u << 8 | 0x0e;
    if (r->is8125)
        rxcfg |= 8u << 27; /* (its receive descriptors fetched eight at a time) */
    else
        rxcfg |= 7u << 13; /* (no receive FIFO threshold) */
    mmio_write32(r->mmio + REG_RXCFG, rxcfg);
    mmio_write32(r->mmio + REG_TXCFG, 3u << 24 | 7u << 8); /* standard gap between frames, unlimited bursts */
    for (int i = 0; i < 8; i++) /* every multicast group (IPv6 neighbour discovery, mDNS) */
        mmio_write8(r->mmio + REG_MAR0 + i, 0xff);
    mmio_write8(r->mmio + REG_9346, 0x00);
    ksnprintf(r->nif.name, sizeof r->nif.name, "eth0");
    r->nif.tx = rtl_tx;
    r->nif.poll = rtl_poll;
    r->nif.link = rtl_link;
    r->nif.drv = r;
    kprintf("r8169: RTL%s (%04x) at %lx, mac %02x:%02x:%02x:%02x:%02x:%02x, link %s\n",
            r->is8125 ? "8125" : "8111/8168", pd->device, pd->bar[bar], r->nif.mac[0], r->nif.mac[1], r->nif.mac[2],
            r->nif.mac[3], r->nif.mac[4], r->nif.mac[5], rtl_link(&r->nif) ? "up" : "down (yet)");
    net_register(&r->nif);
}

/* Bring up the first one found: true if there was one. */
bool r8169_init(void);
bool r8169_init(void) {
    static const uint16_t ids[] = {0x8125, 0x3000 /* (Killer E3000: an 8125) */, 0x8168, 0x8161, 0x8169, 0x8167};
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->vendor != 0x10ec || d->class_code != 0x02)
            continue;
        for (size_t k = 0; k < ARRAY_LEN(ids); k++)
            if (d->device == ids[k]) {
                bring_up(d);
                return true;
            }
    }
    return false;
}
