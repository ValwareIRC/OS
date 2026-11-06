#include <kernel/blkdev.h>
#include <kernel/proc.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/usb.h>

/* USB sticks and drives (mass storage, "bulk-only transport": a SCSI command in a 31-byte wrapper, the data,
 * then a 13-byte status), as /dev/sdX block devices. Each is registered read-only and made writable only if it
 * already is this system's (blkdev_claim_if_ours): someone's backup drive, a camera's card or the stick the
 * system started from are never written. Unplugged, it stays a name that answers every read with an error. */

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u
#define MAX_SECTORS_PER_CMD 128

struct __attribute__((packed)) cbw {
    uint32_t sig, tag, len;
    uint8_t flags, lun, cblen;
    uint8_t cb[16];
};

struct __attribute__((packed)) csw {
    uint32_t sig, tag, residue;
    uint8_t status;
};

struct msc {
    struct blkdev disk;
    struct usb_device *dev;
    int intf;
    uint8_t in, out;
    uint32_t tag;
    bool gone;
};

static void reset_recovery(struct msc *m) {
    usb_control(m->dev, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xff /* Bulk-Only Mass Storage Reset */, 0,
                m->dev->intf[m->intf].number, NULL, 0);
    usb_clear_halt(m->dev, m->in);
    usb_clear_halt(m->dev, m->out);
}

/* One SCSI command: 0, or a negative errno (-EIO when the device said it failed: sense data asked for by the
 * caller if it cares). */
static int scsi(struct msc *m, const uint8_t *cb, int cblen, void *data, uint32_t len, bool in, int timeout_ms) {
    if (m->gone)
        return -ENODEV;
    struct cbw c = {CBW_SIG, ++m->tag, len, (uint8_t)(in ? 0x80 : 0), 0, (uint8_t)cblen, {0}};
    memcpy(c.cb, cb, (size_t)cblen);
    int r = usb_transfer(m->dev, m->out, &c, sizeof c, timeout_ms);
    if (r == -EPIPE) {
        reset_recovery(m);
        return -EIO;
    }
    if (r < 0)
        return r;
    if (len) {
        r = usb_transfer(m->dev, in ? m->in : m->out, data, len, timeout_ms);
        if (r == -EPIPE) /* the data stopped short: the status still follows */
            usb_clear_halt(m->dev, in ? m->in : m->out);
        else if (r < 0)
            return r;
    }
    struct csw s;
    r = usb_transfer(m->dev, m->in, &s, sizeof s, timeout_ms);
    if (r == -EPIPE) {
        usb_clear_halt(m->dev, m->in);
        r = usb_transfer(m->dev, m->in, &s, sizeof s, timeout_ms);
    }
    if (r < (int)sizeof s || s.sig != CSW_SIG || s.tag != c.tag || s.status == 2) {
        if (r != -ENODEV)
            reset_recovery(m);
        return r < 0 ? r : -EIO;
    }
    return s.status == 0 ? 0 : -EIO;
}

static int msc_rw(struct blkdev *d, uint64_t lba, uint64_t count, void *buf, bool write) {
    struct msc *m = d->drv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = count > MAX_SECTORS_PER_CMD ? MAX_SECTORS_PER_CMD : (uint32_t)count;
        uint8_t cb[16] = {0};
        int cl;
        if (lba + n > 0xffffffffull) { /* READ(16) / WRITE(16) */
            cb[0] = write ? 0x8a : 0x88;
            for (int i = 0; i < 8; i++)
                cb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
            for (int i = 0; i < 4; i++)
                cb[10 + i] = (uint8_t)(n >> (24 - 8 * i));
            cl = 16;
        } else {
            cb[0] = write ? 0x2a : 0x28;
            for (int i = 0; i < 4; i++)
                cb[2 + i] = (uint8_t)(lba >> (24 - 8 * i));
            cb[7] = (uint8_t)(n >> 8), cb[8] = (uint8_t)n;
            cl = 10;
        }
        int r = -EIO;
        for (int attempt = 0; attempt < 3 && r == -EIO; attempt++)
            r = scsi(m, cb, cl, p, n * d->sector_size, !write, 10000);
        if (r < 0)
            return r;
        p += (size_t)n * d->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int msc_flush(struct blkdev *d) {
    uint8_t cb[10] = {0x35}; /* SYNCHRONIZE CACHE(10) */
    int r = scsi(d->drv, cb, 10, NULL, 0, false, 30000);
    return r == -EIO ? 0 : r; /* (many sticks don't have the command; they have no cache to drain) */
}

static bool ready(struct msc *m) {
    for (int i = 0; i < 20; i++) { /* a stick can take a few seconds to spin up or settle */
        uint8_t tur[6] = {0x00};
        if (scsi(m, tur, 6, NULL, 0, false, 3000) == 0)
            return true;
        uint8_t sense[18], rs[6] = {0x03, 0, 0, 0, 18, 0};
        if (scsi(m, rs, 6, sense, 18, true, 3000) < 0 && m->gone)
            return false;
        thread_sleep_ms(250);
    }
    return false;
}

static bool probe(struct usb_device *d, int i) {
    struct usb_interface *in = &d->intf[i];
    if (in->cls != USB_CLASS_MASS_STORAGE || in->protocol != 0x50 || (in->subclass != 6 && in->subclass != 5))
        return false;
    struct msc *m = kzalloc(sizeof *m);
    if (!m)
        return false;
    m->dev = d, m->intf = i;
    for (int k = 0; k < in->nendpoints; k++)
        if ((in->ep[k].attributes & 3) == 2) {
            if (in->ep[k].address & USB_DIR_IN)
                m->in = in->ep[k].address;
            else
                m->out = in->ep[k].address;
            usb_enable_endpoint(d, &in->ep[k]);
        }
    if (!m->in || !m->out || !ready(m)) {
        kprintf("usb: %s: a drive that isn't ready (or has nothing in it)\n", d->name);
        kfree(m);
        return false;
    }
    uint8_t inq[36] = {0}, ic[6] = {0x12, 0, 0, 0, 36, 0};
    scsi(m, ic, 6, inq, 36, true, 3000);
    uint8_t cap[32] = {0}, rc[10] = {0x25};
    if (scsi(m, rc, 10, cap, 8, true, 3000) < 0) {
        kfree(m);
        return false;
    }
    uint64_t last = (uint32_t)(cap[0] << 24 | cap[1] << 16 | cap[2] << 8 | cap[3]);
    uint32_t bs = (uint32_t)(cap[4] << 24 | cap[5] << 16 | cap[6] << 8 | cap[7]);
    if (last == 0xffffffffull) { /* over 2 TiB: READ CAPACITY(16) */
        uint8_t rc16[16] = {0x9e, 0x10};
        rc16[13] = 32;
        if (scsi(m, rc16, 16, cap, 32, true, 3000) < 0) {
            kfree(m);
            return false;
        }
        last = 0;
        for (int k = 0; k < 8; k++)
            last = last << 8 | cap[k];
        bs = (uint32_t)(cap[8] << 24 | cap[9] << 16 | cap[10] << 8 | cap[11]);
    }
    if (!bs || bs > 4096 || (bs & (bs - 1))) {
        kprintf("usb: %s: sectors of %u bytes aren't handled\n", d->name, bs);
        kfree(m);
        return false;
    }
    m->disk.sectors = last + 1;
    m->disk.sector_size = bs;
    m->disk.rw = msc_rw;
    m->disk.flush = msc_flush;
    m->disk.drv = m;
    m->disk.readonly = true; /* (until blkdev_claim_if_ours has looked at it) */
    char model[41];
    int k = 0;
    for (int j = 8; j < 36 && k < 40; j++) /* vendor and product, as the drive says them */
        model[k++] = inq[j] >= 32 && inq[j] < 127 ? (char)inq[j] : ' ';
    while (k && model[k - 1] == ' ')
        k--;
    model[k] = 0;
    memcpy(m->disk.model, model, sizeof m->disk.model);
    if (!model[0])
        ksnprintf(m->disk.model, sizeof m->disk.model, "%s", d->name);
    if (blkdev_register(&m->disk) < 0) {
        kfree(m);
        return false;
    }
    d->drv_data[i] = m;
    kprintf("usb: %s: a drive of %lu MiB (/dev/%s)\n", d->name,
            (unsigned long)(m->disk.sectors * m->disk.sector_size >> 20), m->disk.name);
    blkdev_claim_if_ours(&m->disk);
    return true;
}

static void disconnect(struct usb_device *d, int i) {
    struct msc *m = d->drv_data[i];
    if (m) {
        m->gone = true;
        m->disk.readonly = true;
    }
}

static struct usb_driver msc_driver = {.name = "msc", .probe = probe, .disconnect = disconnect};

void usb_msc_init(void);
void usb_msc_init(void) { usb_register_driver(&msc_driver); }
