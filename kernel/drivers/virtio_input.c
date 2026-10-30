#include <asm/io.h>
#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/input.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/virtio.h>

/* virtio-input (virtio-keyboard-pci, virtio-tablet-pci) through the modern
 * (virtio 1.x) PCI interface: register blocks in memory BARs, found through
 * the vendor capabilities. Its events already are Linux input events; a
 * kernel thread polls the event queues and feeds them to the evdev devices.
 * A device with pointer axes or buttons is the mouse, anything else the
 * keyboard. */

#define VIRTIO_INPUT_MODERN 0x1052

#define CAP_COMMON 1
#define CAP_NOTIFY 2
#define CAP_DEVICE 4

/* common configuration */
#define CC_DEVICE_FEATURE_SELECT 0x00
#define CC_DEVICE_FEATURE        0x04
#define CC_DRIVER_FEATURE_SELECT 0x08
#define CC_DRIVER_FEATURE        0x0c
#define CC_STATUS                0x14
#define CC_QUEUE_SELECT          0x16
#define CC_QUEUE_SIZE            0x18
#define CC_QUEUE_ENABLE          0x1c
#define CC_QUEUE_NOTIFY_OFF      0x1e
#define CC_QUEUE_DESC            0x20
#define CC_QUEUE_DRIVER          0x28
#define CC_QUEUE_DEVICE          0x30

#define F_VERSION_1_HI 1 /* feature bit 32 */

/* device configuration */
#define CFG_SELECT   0x00
#define CFG_SUBSEL   0x01
#define CFG_SIZE     0x02
#define CFG_DATA     0x08
#define CFG_EV_BITS  0x11
#define CFG_ABS_INFO 0x12

#define QSIZE 64
#define POLL_MS 8

struct vinput_event {
    uint16_t type, code;
    int32_t value;
};

struct vinput {
    volatile uint8_t *common, *device;
    volatile uint16_t *notify;
    struct evdev *dev;
    int32_t abs_max[2];
    struct virtq q;
    struct vinput_event *ev;
    struct vinput *next;
};

static struct vinput *devices;

static volatile uint8_t *cap_map(struct pci_dev *pd, uint8_t cap, uint32_t *len) {
    uint8_t bar = pci_read32(pd, (uint8_t)(cap + 4)) & 0xff;
    uint32_t off = pci_read32(pd, (uint8_t)(cap + 8));
    *len = pci_read32(pd, (uint8_t)(cap + 12));
    if (bar > 5 || pd->bar_is_io[bar] || !pd->bar[bar])
        return NULL;
    vmm_map_mmio(pd->bar[bar] + off, *len, true);
    return phys_to_virt(pd->bar[bar] + off);
}

static uint8_t cfg_query(struct vinput *v, uint8_t select, uint8_t subsel) {
    mmio_write8(v->device + CFG_SELECT, select);
    mmio_write8(v->device + CFG_SUBSEL, subsel);
    return mmio_read8(v->device + CFG_SIZE);
}

static void q64(struct vinput *v, uint16_t reg, uint64_t val) {
    mmio_write32(v->common + reg, (uint32_t)val);
    mmio_write32(v->common + reg + 4, (uint32_t)(val >> 32));
}

static void handle(struct vinput *v, struct vinput_event *e) {
    if (v->dev == &input_kbd && !evdev_is_open(&input_kbd))
        return;
    if (e->type == EV_SYN) {
        evdev_sync(v->dev);
    } else if (e->type == EV_ABS && e->code < 2) {
        /* We scale to 0..0xffff, the range the vmmouse pointer reports. */
        evdev_push(v->dev, EV_ABS, e->code, (int32_t)((int64_t)e->value * 0xffff / v->abs_max[e->code]));
    } else if (e->type == EV_KEY || e->type == EV_REL) {
        evdev_push(v->dev, e->type, e->code, e->value);
    }
}

static void poll_thread(void *arg) {
    (void)arg;
    for (;;) {
        for (struct vinput *v = devices; v; v = v->next) {
            bool refilled = false;
            for (int id; (id = virtq_next_used(&v->q)) >= 0;) {
                handle(v, &v->ev[id]);
                v->q.avail->ring[v->q.avail_idx % QSIZE] = (uint16_t)id;
                dma_wmb();
                v->q.avail->idx = ++v->q.avail_idx;
                refilled = true;
            }
            if (refilled) {
                dma_wmb();
                mmio_write16(v->notify, 0);
            }
        }
        thread_sleep_ms(POLL_MS);
    }
}

static bool setup_queue(struct vinput *v) {
    mmio_write16(v->common + CC_QUEUE_SELECT, 0);
    if (mmio_read16(v->common + CC_QUEUE_SIZE) < QSIZE)
        return false;
    mmio_write16(v->common + CC_QUEUE_SIZE, QSIZE);
    /* One page: descriptors, available ring, used ring, then the event buffers. */
    uint64_t phys = pmm_alloc(1);
    if (!phys)
        return false;
    uint8_t *page = phys_to_virt(phys);
    virtq_carve(&v->q, page, QSIZE, 4);
    v->ev = (struct vinput_event *)(page + 3072);
    v->q.avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* we poll */
    for (int i = 0; i < QSIZE; i++) {
        v->q.desc[i] = (struct vring_desc){phys + 3072 + (uint64_t)i * sizeof *v->ev, sizeof *v->ev, VRING_DESC_F_WRITE, 0};
        v->q.avail->ring[i] = (uint16_t)i;
    }
    v->q.avail_idx = QSIZE;
    v->q.avail->idx = QSIZE;
    q64(v, CC_QUEUE_DESC, phys);
    q64(v, CC_QUEUE_DRIVER, phys + (uint64_t)((uint8_t *)v->q.avail - page));
    q64(v, CC_QUEUE_DEVICE, phys + (uint64_t)((uint8_t *)v->q.used - page));
    mmio_write16(v->common + CC_QUEUE_ENABLE, 1);
    return true;
}

static void bring_up(struct pci_dev *pd) {
    struct vinput *v = kzalloc(sizeof *v);
    if (!v)
        return;
    pci_enable(pd);
    volatile uint8_t *notify_base = NULL;
    uint32_t notify_mult = 0, len;
    /* Vendor-specific capabilities: the virtio register blocks. */
    for (uint8_t cap = pci_find_cap(pd, 0x09, 0); cap; cap = pci_find_cap(pd, 0x09, cap)) {
        uint8_t type = pci_read32(pd, cap) >> 24;
        if (type == CAP_COMMON)
            v->common = cap_map(pd, cap, &len);
        else if (type == CAP_DEVICE)
            v->device = cap_map(pd, cap, &len);
        else if (type == CAP_NOTIFY) {
            notify_base = cap_map(pd, cap, &len);
            notify_mult = pci_read32(pd, (uint8_t)(cap + 16));
        }
    }
    if (!v->common || !v->device || !notify_base) {
        kprintf("virtio-input: %02x:%02x.%u has no modern interface\n", pd->bus, pd->dev, pd->fn);
        kfree(v);
        return;
    }
    mmio_write8(v->common + CC_STATUS, 0);
    uint64_t reset_end = time_monotonic_ns() + 100 * 1000000ULL;
    while (mmio_read8(v->common + CC_STATUS)) {
        if (time_monotonic_ns() > reset_end) {
            kprintf("virtio-input: %02x:%02x.%u does not reset\n", pd->bus, pd->dev, pd->fn);
            kfree(v);
            return;
        }
        cpu_relax();
    }
    mmio_write8(v->common + CC_STATUS, STATUS_ACK);
    mmio_write8(v->common + CC_STATUS, STATUS_ACK | STATUS_DRIVER);
    mmio_write32(v->common + CC_DEVICE_FEATURE_SELECT, 1);
    bool v1 = mmio_read32(v->common + CC_DEVICE_FEATURE) & F_VERSION_1_HI;
    mmio_write32(v->common + CC_DRIVER_FEATURE_SELECT, 0);
    mmio_write32(v->common + CC_DRIVER_FEATURE, 0);
    mmio_write32(v->common + CC_DRIVER_FEATURE_SELECT, 1);
    mmio_write32(v->common + CC_DRIVER_FEATURE, v1 ? F_VERSION_1_HI : 0);
    mmio_write8(v->common + CC_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK);
    if (!v1 || !(mmio_read8(v->common + CC_STATUS) & STATUS_FEATURES_OK) || !setup_queue(v)) {
        mmio_write8(v->common + CC_STATUS, STATUS_FAILED);
        kprintf("virtio-input: %02x:%02x.%u: cannot set up the device\n", pd->bus, pd->dev, pd->fn);
        kfree(v);
        return;
    }
    v->notify = (volatile uint16_t *)(notify_base + mmio_read16(v->common + CC_QUEUE_NOTIFY_OFF) * notify_mult);

    bool pointer = cfg_query(v, CFG_EV_BITS, EV_ABS) || cfg_query(v, CFG_EV_BITS, EV_REL);
    v->dev = pointer ? &input_mouse : &input_kbd;
    for (uint8_t axis = 0; axis < 2; axis++) {
        v->abs_max[axis] = cfg_query(v, CFG_ABS_INFO, axis) ? (int32_t)mmio_read32(v->device + CFG_DATA + 4) : 0;
        if (v->abs_max[axis] <= 0)
            v->abs_max[axis] = 0xffff;
    }
    char name[32] = {0};
    uint8_t n = cfg_query(v, 0x01 /* ID_NAME */, 0);
    for (uint8_t i = 0; i < n && i < sizeof name - 1; i++)
        name[i] = (char)mmio_read8(v->device + CFG_DATA + i);
    mmio_write8(v->common + CC_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
    dma_wmb();
    mmio_write16(v->notify, 0);
    v->next = devices;
    devices = v;
    kprintf("virtio-input: %02x:%02x.%u: %s, %s\n", pd->bus, pd->dev, pd->fn, name, pointer ? "pointer" : "keyboard");
}

void virtio_input_init(void) {
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        if (d->vendor == VIRTIO_VENDOR && d->device == VIRTIO_INPUT_MODERN)
            bring_up(d);
    }
    if (devices)
        KASSERT(thread_create("virtio-input", poll_thread, NULL));
}
