#include <asm/io.h>
#include <kernel/blkdev.h>
#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>

/* NVMe (NVM Express): the SSDs of nearly every computer made since about 2016, all through one standard
 * interface. Commands are 64-byte entries put on a submission queue in memory; the drive answers with 16-byte
 * entries on a completion queue; a doorbell register says where each queue's end is. Here: the admin queue
 * (identify, make queues) and one queue pair for reading and writing, a command at a time under the block
 * layer's per-disk lock, polled to completion (as ahci.c). Every namespace is a disk, /dev/sdX; as with every
 * disk (blkdev.c), one that isn't already this system's is read-only: a Windows SSD is never written. */

#define REG_CAP 0x00
#define REG_CC 0x14
#define REG_CSTS 0x1c
#define REG_AQA 0x24
#define REG_ASQ 0x28
#define REG_ACQ 0x30

#define CC_EN (1u << 0)
#define CC_SHN_NORMAL (1u << 14)
#define CSTS_RDY (1u << 0)
#define CSTS_CFS (1u << 1)

#define ADMIN_DELETE_SQ 0x00
#define ADMIN_CREATE_SQ 0x01
#define ADMIN_CREATE_CQ 0x05
#define ADMIN_IDENTIFY 0x06
#define IO_FLUSH 0x00
#define IO_WRITE 0x01
#define IO_READ 0x02

#define QSIZE 32
#define MAX_XFER (128u * 1024) /* per command: the bounce buffer's size (and well under any drive's limit) */
#define CMD_TIMEOUT_MS 30000

struct sqe {
    uint32_t cdw0, nsid, rsvd[2];
    uint64_t mptr, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    volatile uint16_t status; /* phase in bit 0 */
};

struct queue {
    struct sqe *sq;
    struct cqe *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t tail, head, cid;
    uint8_t phase;
    volatile uint32_t *sq_db, *cq_db;
};

struct nvme {
    volatile uint8_t *regs;
    uint32_t stride;
    struct queue admin, io;
    uint8_t *bounce; /* MAX_XFER, contiguous */
    uint64_t bounce_phys;
    uint64_t *prp_list; /* the bounce buffer's pages after the first */
    uint64_t prp_list_phys;
    uint32_t max_xfer;
    volatile int busy; /* the io queue, shared by its namespaces */
    char model[41];
    int index;
};

struct nvme_ns {
    struct blkdev bd;
    struct nvme *c;
    uint32_t nsid;
};

static int ncontrollers;

static void *page(uint64_t *phys, size_t pages) {
    uint64_t p = pmm_alloc(pages);
    if (!p)
        return NULL;
    void *v = phys_to_virt(p);
    memset(v, 0, pages * PAGE_SIZE);
    *phys = p;
    return v;
}

static bool wait_ready(struct nvme *c, bool want, uint32_t ms) {
    uint64_t end = time_monotonic_ns() + (uint64_t)ms * 1000000ull;
    while (time_monotonic_ns() < end) {
        uint32_t s = mmio_read32(c->regs + REG_CSTS);
        if (s & CSTS_CFS)
            return false;
        if (!!(s & CSTS_RDY) == want)
            return true;
        thread_sleep_ms(1);
    }
    return false;
}

static bool queue_init(struct nvme *c, struct queue *q, int id) {
    q->sq = page(&q->sq_phys, 1); /* 32 x 64 bytes */
    q->cq = page(&q->cq_phys, 1); /* 32 x 16 bytes */
    q->phase = 1;
    q->sq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * id) * c->stride);
    q->cq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * id + 1) * c->stride);
    return q->sq && q->cq;
}

/* One command, waited for: its status (0 success), or -1 if the drive never answered. */
static int submit(struct nvme *c, struct queue *q, struct sqe *cmd, uint32_t *dw0) {
    uint16_t cid = ++q->cid;
    cmd->cdw0 = (cmd->cdw0 & 0xffff) | (uint32_t)cid << 16;
    q->sq[q->tail] = *cmd;
    q->tail = (uint16_t)((q->tail + 1) % QSIZE);
    dma_wmb();
    mmio_write32(q->sq_db, q->tail);
    uint64_t end = time_monotonic_ns() + CMD_TIMEOUT_MS * 1000000ull;
    for (uint32_t spins = 0;; spins++) {
        struct cqe *e = &q->cq[q->head];
        if ((e->status & 1) == q->phase) {
            dma_rmb();
            int status = e->status >> 1;
            uint16_t got = e->cid;
            if (dw0)
                *dw0 = e->dw0;
            if (++q->head == QSIZE) {
                q->head = 0;
                q->phase ^= 1;
            }
            mmio_write32(q->cq_db, q->head);
            if (got == cid)
                return status & 0x7fff;
            continue; /* (an old one, from before a timeout) */
        }
        if (time_monotonic_ns() > end) {
            kprintf("nvme%d: a command had no answer\n", c->index);
            return -1;
        }
        if (spins > 2000)
            thread_sleep_ms(1);
    }
}

static int identify(struct nvme *c, uint32_t cns, uint32_t nsid, void *out) {
    struct sqe cmd = {.cdw0 = ADMIN_IDENTIFY, .nsid = nsid, .prp1 = c->bounce_phys, .cdw10 = cns};
    int r = submit(c, &c->admin, &cmd, NULL);
    if (r == 0)
        memcpy(out, c->bounce, 4096);
    return r;
}

static void lock(struct nvme *c) {
    while (__atomic_exchange_n(&c->busy, 1, __ATOMIC_ACQUIRE))
        thread_sleep_ms(1);
}

static void unlock(struct nvme *c) { __atomic_store_n(&c->busy, 0, __ATOMIC_RELEASE); }

static int nvme_rw(struct blkdev *bd, uint64_t lba, uint64_t count, void *buf, bool write) {
    struct nvme_ns *ns = bd->drv;
    struct nvme *c = ns->c;
    if (lba + count > bd->sectors || lba + count < lba)
        return -EIO;
    uint8_t *p = buf;
    uint32_t per = c->max_xfer / bd->sector_size;
    while (count) {
        uint32_t n = count > per ? per : (uint32_t)count;
        size_t bytes = (size_t)n * bd->sector_size;
        lock(c);
        if (write)
            memcpy(c->bounce, p, bytes);
        struct sqe cmd = {.cdw0 = write ? IO_WRITE : IO_READ, .nsid = ns->nsid, .prp1 = c->bounce_phys};
        size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        cmd.prp2 = pages == 2 ? c->bounce_phys + PAGE_SIZE : pages > 2 ? c->prp_list_phys : 0;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = n - 1;
        int r = submit(c, &c->io, &cmd, NULL);
        if (!r && !write)
            memcpy(p, c->bounce, bytes);
        unlock(c);
        if (r) {
            kprintf("nvme%d: %s at %lu failed (%d)\n", c->index, write ? "a write" : "a read", (unsigned long)lba, r);
            return -EIO;
        }
        p += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

static int nvme_flush(struct blkdev *bd) {
    struct nvme_ns *ns = bd->drv;
    lock(ns->c);
    struct sqe cmd = {.cdw0 = IO_FLUSH, .nsid = ns->nsid};
    int r = submit(ns->c, &ns->c->io, &cmd, NULL);
    unlock(ns->c);
    return r ? -EIO : 0;
}

static void add_namespace(struct nvme *c, uint32_t nsid, const uint8_t *id) {
    uint64_t sectors = 0;
    memcpy(&sectors, id, 8); /* NSZE */
    uint8_t format = id[26] & 0xf;
    uint8_t lbads = id[128 + 4 * format + 2];
    if (!sectors || lbads < 9 || lbads > 12) {
        if (sectors)
            kprintf("nvme%d: namespace %u has sectors of 2^%u bytes: not handled\n", c->index, nsid, lbads);
        return;
    }
    struct nvme_ns *ns = kzalloc(sizeof *ns);
    if (!ns)
        return;
    ns->c = c;
    ns->nsid = nsid;
    ns->bd.sectors = sectors;
    ns->bd.sector_size = 1u << lbads;
    ns->bd.rw = nvme_rw;
    ns->bd.flush = nvme_flush;
    ns->bd.drv = ns;
    memcpy(ns->bd.model, c->model, sizeof ns->bd.model);
    if (blkdev_register(&ns->bd) < 0) {
        kfree(ns);
        return;
    }
    kprintf("nvme%d: %s, namespace %u: %lu MiB (/dev/%s)\n", c->index, c->model, nsid,
            (unsigned long)(sectors * ns->bd.sector_size >> 20), ns->bd.name);
}

static void bring_up(struct pci_dev *pd) {
    if (pd->bar_is_io[0] || !pd->bar[0])
        return;
    struct nvme *c = kzalloc(sizeof *c);
    if (!c)
        return;
    c->index = ncontrollers++;
    pci_enable(pd);
    vmm_map_mmio(pd->bar[0], pd->bar_size[0] ? pd->bar_size[0] : 0x4000, true);
    c->regs = phys_to_virt(pd->bar[0]);
    uint64_t cap = mmio_read64(c->regs + REG_CAP);
    c->stride = 4u << ((cap >> 32) & 0xf);
    uint32_t timeout = (uint32_t)((cap >> 24) & 0xff) * 500 + 1000;
    uint32_t mqes = (uint32_t)(cap & 0xffff) + 1;
    if (mqes < QSIZE || (cap >> 37 & 1) == 0 || ((cap >> 48) & 0xf) > 0) {
        kprintf("nvme%d: its queues or pages aren't the kind handled here; left alone\n", c->index);
        return;
    }
    /* stopped (the firmware may have been using it), then the admin queues, then started */
    mmio_write32(c->regs + REG_CC, mmio_read32(c->regs + REG_CC) & ~CC_EN);
    if (!wait_ready(c, false, timeout)) {
        kprintf("nvme%d: it didn't stop\n", c->index);
        return;
    }
    c->bounce = page(&c->bounce_phys, MAX_XFER / PAGE_SIZE);
    c->prp_list = page(&c->prp_list_phys, 1);
    if (!c->bounce || !c->prp_list || !queue_init(c, &c->admin, 0) || !queue_init(c, &c->io, 1))
        return;
    for (uint32_t i = 1; i < MAX_XFER / PAGE_SIZE; i++)
        c->prp_list[i - 1] = c->bounce_phys + (uint64_t)i * PAGE_SIZE;
    mmio_write32(c->regs + REG_AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
    mmio_write64(c->regs + REG_ASQ, c->admin.sq_phys);
    mmio_write64(c->regs + REG_ACQ, c->admin.cq_phys);
    mmio_write32(c->regs + REG_CC, CC_EN | 6u << 16 | 4u << 20); /* 64-byte commands, 16-byte answers, 4 KiB pages */
    if (!wait_ready(c, true, timeout)) {
        kprintf("nvme%d: it didn't start\n", c->index);
        return;
    }
    static uint8_t id[4096];
    if (identify(c, 1, 0, id)) {
        kprintf("nvme%d: it didn't say what it is\n", c->index);
        return;
    }
    int k = 0;
    for (int i = 24; i < 64 && k < 40; i++)
        c->model[k++] = id[i] >= 32 && id[i] < 127 ? (char)id[i] : ' ';
    while (k && c->model[k - 1] == ' ')
        k--;
    c->model[k] = 0;
    c->max_xfer = MAX_XFER;
    if (id[77]) { /* its own limit, in pages */
        uint64_t lim = (uint64_t)PAGE_SIZE << id[77];
        if (lim < c->max_xfer)
            c->max_xfer = (uint32_t)lim;
    }
    uint32_t nn;
    memcpy(&nn, id + 516, 4);
    /* the queue pair for reading and writing */
    struct sqe cq = {.cdw0 = ADMIN_CREATE_CQ, .prp1 = c->io.cq_phys, .cdw10 = (QSIZE - 1) << 16 | 1, .cdw11 = 1};
    struct sqe sq = {.cdw0 = ADMIN_CREATE_SQ, .prp1 = c->io.sq_phys, .cdw10 = (QSIZE - 1) << 16 | 1, .cdw11 = 1u << 16 | 1};
    if (submit(c, &c->admin, &cq, NULL) || submit(c, &c->admin, &sq, NULL)) {
        kprintf("nvme%d: its queues couldn't be made\n", c->index);
        return;
    }
    for (uint32_t ns = 1; ns <= nn && ns <= 8; ns++)
        if (!identify(c, 0, ns, id))
            add_namespace(c, ns, id);
}

void nvme_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->class_code == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02)
            bring_up(d);
    }
}
