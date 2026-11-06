#include <kernel/input.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/usb.h>

/* USB keyboards, mice and tablets (HID, "human interface devices"): every one describes its own reports (which
 * bits are which keys, axes and buttons) in a report descriptor; that is read here, and each report coming in is
 * turned into Linux input events for /dev/input/event0 (keys) and event1 (the pointer). Keys come from the
 * keyboard page (letters, modifiers, either the 6-key array or a bitmap of every key) and the consumer page
 * (volume, media); the pointer from the generic desktop page (X, Y, wheel: relative for a mouse, absolute for a
 * tablet or a touch screen) and the button page. */

#define REL_X 0
#define REL_Y 1
#define REL_HWHEEL 6
#define REL_WHEEL 8
#define ABS_X 0
#define ABS_Y 1

#define PAGE_DESKTOP 0x01
#define PAGE_KEYBOARD 0x07
#define PAGE_BUTTON 0x09
#define PAGE_CONSUMER 0x0c

/* the keyboard page's usages as Linux key codes (as Linux's own hid-input does) */
static const uint8_t usage_to_key[256] = {
    0,   0,   0,   0,   30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,  /* a b c d ... */
    50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,  2,   3,   /* ... z 1 2 */
    4,   5,   6,   7,   8,   9,   10,  11,  28,  1,   14,  15,  57,  12,  13,  26,  /* 3..0 enter esc bksp tab space - = [ */
    27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,  /* ] \ # ; ' ` , . / caps F1..F6 */
    65,  66,  67,  68,  87,  88,  99,  70,  119, 110, 102, 104, 111, 107, 109, 106, /* F7..F12 prtsc scroll pause ins.. */
    105, 108, 103, 69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,  /* left down up numlk kp/ * - + ... */
    72,  73,  82,  83,  86,  127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190, /* ... kp0 kp. 102nd compose power */
    191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113, /* F21.. open help props .. mute */
    115, 114, 0,   0,   0,   121, 0,   89,  93,  124, 92,  94,  95,  0,   0,   0,   /* vol+ vol- ... ro katakana yen */
    122, 123, 90,  91,  85,  0,   0,   0,   0,   0,   0,   0,   111, 0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   179, 180, 0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   111, 0,   0,   0,   0,   0,   0,   0,
    29,  42,  56,  125, 97,  54,  100, 126, 164, 166, 165, 163, 161, 115, 114, 113, /* ctrl shift alt meta (left, right) */
    150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140, 0,   0,   0,   0,
};

/* the consumer page's usual keys */
static uint16_t consumer_key(uint16_t u) {
    switch (u) {
    case 0xe2: return 113; /* mute */
    case 0xe9: return 115; /* volume up */
    case 0xea: return 114; /* volume down */
    case 0xcd: return 164; /* play/pause */
    case 0xb5: return 163; /* next */
    case 0xb6: return 165; /* previous */
    case 0xb7: return 166; /* stop */
    case 0x6f: return 225; /* brightness up */
    case 0x70: return 224; /* brightness down */
    case 0x223: return 172; /* home (browser) */
    case 0x224: return 158; /* back */
    case 0x192: return 140; /* calculator */
    }
    return 0;
}

struct field {
    uint8_t id;
    uint16_t page, usage, usage_max;
    uint32_t bit;
    uint8_t size, count;
    bool array, relative;
    int32_t lmin, lmax;
};

#define MAX_FIELDS 96
#define MAX_KEYS 32

struct hid {
    struct usb_device *dev;
    int intf;
    struct field f[MAX_FIELDS];
    int nf;
    bool ids;           /* reports start with their ID */
    uint16_t report_len;
    uint16_t keys[MAX_KEYS]; /* down now (Linux codes) */
    int nkeys;
    uint8_t buttons;
    bool pointer, keyboard;
    struct usb_endpoint ep;
};

static uint32_t bits_at(const uint8_t *d, int len, uint32_t bit, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size && i < 32; i++) {
        uint32_t b = bit + i;
        if ((int)(b / 8) >= len)
            break;
        v |= (uint32_t)((d[b / 8] >> (b % 8)) & 1) << i;
    }
    return v;
}

static int32_t value_of(const struct field *f, uint32_t raw) {
    if (f->lmin < 0 && f->size < 32 && (raw & (1u << (f->size - 1))))
        return (int32_t)(raw | ~((1u << f->size) - 1)); /* (signed) */
    return (int32_t)raw;
}

/* ---- the report descriptor ---- */

static void add_field(struct hid *h, struct field f) {
    if (h->nf < MAX_FIELDS)
        h->f[h->nf++] = f;
}

static bool parse(struct hid *h, const uint8_t *p, int len) {
    struct {
        uint16_t page;
        int32_t lmin, lmax;
        uint32_t size, count;
        uint8_t id;
    } g = {0}, stack[4];
    int sp = 0;
    uint32_t usages[32];
    int nu = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    static uint32_t off[256];
    memset(off, 0, sizeof off);
    uint32_t longest = 0;
    for (int i = 0; i < len;) {
        uint8_t pre = p[i];
        if (pre == 0xfe) { /* a long item: skipped */
            if (i + 2 >= len)
                break;
            i += 3 + p[i + 1];
            continue;
        }
        int n = (pre & 3) == 3 ? 4 : (pre & 3);
        if (i + 1 + n > len)
            break;
        uint32_t u = 0;
        for (int k = 0; k < n; k++)
            u |= (uint32_t)p[i + 1 + k] << (8 * k);
        int32_t sv = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        uint8_t type = (pre >> 2) & 3, tag = pre >> 4;
        i += 1 + n;
        if (type == 1) { /* global */
            switch (tag) {
            case 0: g.page = (uint16_t)u; break;
            case 1: g.lmin = sv; break;
            case 2: g.lmax = n == 4 || g.lmin < 0 ? sv : (int32_t)u; break;
            case 7: g.size = u; break;
            case 8: g.id = (uint8_t)u, h->ids = true; break;
            case 9: g.count = u; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp) g = stack[--sp]; break;
            }
        } else if (type == 2) { /* local */
            if (tag == 0 && nu < 32)
                usages[nu++] = n == 4 ? u : (uint32_t)g.page << 16 | u;
            else if (tag == 1)
                umin = n == 4 ? u : (uint32_t)g.page << 16 | u, range = true;
            else if (tag == 2)
                umax = n == 4 ? u : (uint32_t)g.page << 16 | u, range = true;
        } else if (type == 0) { /* main */
            if (tag == 8 && g.size && g.size <= 32) { /* Input */
                bool constant = u & 1, variable = u & 2, relative = u & 4;
                if (constant) {
                    off[g.id] += g.size * g.count;
                } else if (variable) {
                    for (uint32_t k = 0; k < g.count && k < 64; k++) {
                        uint32_t us = nu ? usages[k < (uint32_t)nu ? k : (uint32_t)nu - 1] : range ? umin + k : 0;
                        if (range && !nu && umin + k > umax)
                            us = umax;
                        add_field(h, (struct field){g.id, (uint16_t)(us >> 16), (uint16_t)us, (uint16_t)us,
                                                    off[g.id], (uint8_t)g.size, 1, false, relative, g.lmin, g.lmax});
                        off[g.id] += g.size;
                    }
                } else {
                    uint32_t lo = range ? umin : nu ? usages[0] : 0, hi = range ? umax : nu ? usages[nu - 1] : 0;
                    add_field(h, (struct field){g.id, (uint16_t)(lo >> 16), (uint16_t)lo, (uint16_t)hi, off[g.id],
                                                (uint8_t)g.size, (uint8_t)(g.count > 255 ? 255 : g.count), true,
                                                false, g.lmin, g.lmax});
                    off[g.id] += g.size * g.count;
                }
                if (off[g.id] > longest)
                    longest = off[g.id];
            } else if (tag == 9 || tag == 11) { /* Output, Feature: not ours to read */
            }
            nu = 0, range = false, umin = umax = 0;
        }
    }
    h->report_len = (uint16_t)((longest + 7) / 8 + (h->ids ? 1 : 0));
    return h->nf > 0;
}

/* ---- reports ---- */

static void key_set(struct hid *h, uint16_t key, uint16_t *now, int *nnow) {
    (void)h;
    if (!key)
        return;
    for (int i = 0; i < *nnow; i++)
        if (now[i] == key)
            return;
    if (*nnow < MAX_KEYS)
        now[(*nnow)++] = key;
}

static uint16_t key_of(uint16_t page, uint32_t usage) {
    if (page == PAGE_KEYBOARD)
        return usage < 256 ? usage_to_key[usage] : 0;
    if (page == PAGE_CONSUMER)
        return consumer_key((uint16_t)usage);
    return 0;
}

static void report(struct usb_device *d, void *arg, const uint8_t *data, int len) {
    (void)d;
    struct hid *h = arg;
    uint8_t id = 0;
    if (h->ids && len > 0) {
        id = data[0];
        data++, len--;
    }
    bool any_keys = false, moved = false;
    uint16_t now[MAX_KEYS];
    int nnow = 0;
    uint8_t buttons = h->buttons;
    bool buttons_seen = false;
    for (int i = 0; i < h->nf; i++) {
        struct field *f = &h->f[i];
        if (f->id != id)
            continue;
        if (f->page == PAGE_KEYBOARD || f->page == PAGE_CONSUMER) {
            any_keys = true;
            if (f->array) {
                for (int k = 0; k < f->count; k++) {
                    uint32_t v = bits_at(data, len, f->bit + (uint32_t)k * f->size, f->size);
                    if (f->page == PAGE_KEYBOARD && v >= 1 && v <= 3) /* "too many keys": keep what was down */
                        return;
                    if (v && (int32_t)v >= f->lmin && (int32_t)v <= f->lmax)
                        key_set(h, key_of(f->page, f->usage + v - (uint32_t)(f->lmin > 0 ? f->lmin : 0)), now, &nnow);
                }
            } else if (bits_at(data, len, f->bit, f->size)) {
                key_set(h, key_of(f->page, f->usage), now, &nnow);
            }
        } else if (f->page == PAGE_BUTTON && !f->array && f->usage >= 1 && f->usage <= 8) {
            buttons_seen = true;
            uint8_t bit = (uint8_t)(1u << (f->usage - 1));
            buttons = bits_at(data, len, f->bit, f->size) ? (buttons | bit) : (buttons & ~bit);
        } else if (f->page == PAGE_DESKTOP && !f->array && (f->usage == 0x30 || f->usage == 0x31 || f->usage == 0x38)) {
            int32_t v = value_of(f, bits_at(data, len, f->bit, f->size));
            if (f->relative) {
                if (v) {
                    evdev_push(&input_mouse, EV_REL, f->usage == 0x30 ? REL_X : f->usage == 0x31 ? REL_Y : REL_WHEEL, v);
                    moved = true;
                }
            } else if (f->usage != 0x38 && f->lmax > f->lmin) { /* a tablet or touch screen: 0..0xffff */
                int64_t s = ((int64_t)v - f->lmin) * 0xffff / ((int64_t)f->lmax - f->lmin);
                evdev_push(&input_mouse, EV_ABS, f->usage == 0x30 ? ABS_X : ABS_Y, (int32_t)(s < 0 ? 0 : s > 0xffff ? 0xffff : s));
                moved = true;
            }
        } else if (f->page == PAGE_CONSUMER && f->usage == 0x238 && f->relative) { /* AC Pan: sideways scrolling */
            int32_t v = value_of(f, bits_at(data, len, f->bit, f->size));
            if (v) {
                evdev_push(&input_mouse, EV_REL, REL_HWHEEL, v);
                moved = true;
            }
        }
    }
    if (buttons_seen && buttons != h->buttons) {
        static const uint16_t code[8] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, 0x113, 0x114, 0x115, 0x116, 0x117};
        for (int b = 0; b < 8; b++)
            if ((buttons ^ h->buttons) & (1u << b))
                evdev_push(&input_mouse, EV_KEY, code[b], (buttons >> b) & 1);
        h->buttons = buttons;
        moved = true;
    }
    if (moved)
        evdev_sync(&input_mouse);
    if (!any_keys)
        return;
    bool changed = false;
    for (int i = 0; i < h->nkeys; i++) { /* let go */
        bool still = false;
        for (int k = 0; k < nnow && !still; k++)
            still = now[k] == h->keys[i];
        if (!still) {
            evdev_push(&input_kbd, EV_KEY, h->keys[i], 0);
            changed = true;
        }
    }
    for (int k = 0; k < nnow; k++) { /* pressed */
        bool was = false;
        for (int i = 0; i < h->nkeys && !was; i++)
            was = h->keys[i] == now[k];
        if (!was) {
            evdev_push(&input_kbd, EV_KEY, now[k], 1);
            changed = true;
        }
    }
    memcpy(h->keys, now, sizeof now[0] * (size_t)nnow);
    h->nkeys = nnow;
    if (changed)
        evdev_sync(&input_kbd);
}

/* The boot protocol's fixed reports, for a device whose own description couldn't be read. */
static void boot_fields(struct hid *h, bool keyboard) {
    h->nf = 0;
    h->ids = false;
    if (keyboard) {
        for (int i = 0; i < 8; i++)
            add_field(h, (struct field){0, PAGE_KEYBOARD, (uint16_t)(0xe0 + i), (uint16_t)(0xe0 + i), (uint32_t)i, 1, 1,
                                        false, false, 0, 1});
        add_field(h, (struct field){0, PAGE_KEYBOARD, 0, 255, 16, 8, 6, true, false, 0, 255});
        h->report_len = 8;
    } else {
        for (int i = 0; i < 3; i++)
            add_field(h, (struct field){0, PAGE_BUTTON, (uint16_t)(i + 1), (uint16_t)(i + 1), (uint32_t)i, 1, 1, false,
                                        false, 0, 1});
        add_field(h, (struct field){0, PAGE_DESKTOP, 0x30, 0x30, 8, 8, 1, false, true, -127, 127});
        add_field(h, (struct field){0, PAGE_DESKTOP, 0x31, 0x31, 16, 8, 1, false, true, -127, 127});
        h->report_len = 3;
    }
}

static bool probe(struct usb_device *d, int i) {
    struct usb_interface *in = &d->intf[i];
    if (in->cls != USB_CLASS_HID)
        return false;
    const struct usb_endpoint *ep = NULL;
    for (int k = 0; k < in->nendpoints; k++)
        if ((in->ep[k].attributes & 3) == 3 && (in->ep[k].address & USB_DIR_IN))
            ep = &in->ep[k];
    if (!ep)
        return false;
    struct hid *h = kzalloc(sizeof *h);
    if (!h)
        return false;
    h->dev = d, h->intf = i, h->ep = *ep;
    /* the report descriptor's length, from the HID descriptor before the endpoints */
    int rlen = 0;
    for (const uint8_t *p = in->extra; p && p + 9 <= in->extra + in->extra_len; p += p[0] ? p[0] : 1)
        if (p[1] == USB_DT_HID && p[0] >= 9 && p[6] == USB_DT_REPORT)
            rlen = p[7] | p[8] << 8;
    if (rlen <= 0 || rlen > 4096)
        rlen = 4096;
    uint8_t *rd = kmalloc((size_t)rlen);
    int got = rd ? usb_control(d, USB_DIR_IN | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8,
                               in->number, rd, (uint16_t)rlen)
                 : -1;
    bool boot = in->subclass == 1;
    if (got <= 0 || !parse(h, rd, got)) {
        if (!boot) {
            kfree(rd), kfree(h);
            return false;
        }
        boot_fields(h, in->protocol == 1);
        usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0x0b /* SET_PROTOCOL */, 0 /* boot */, in->number, NULL, 0);
    }
    kfree(rd);
    for (int k = 0; k < h->nf; k++) {
        h->keyboard |= h->f[k].page == PAGE_KEYBOARD;
        h->pointer |= h->f[k].page == PAGE_BUTTON || (h->f[k].page == PAGE_DESKTOP && h->f[k].usage == 0x30);
    }
    if (!h->keyboard && !h->pointer && !(h->nf && h->f[0].page == PAGE_CONSUMER)) {
        kfree(h); /* (a game pad, a UPS, a sensor: not for these two devices) */
        return false;
    }
    if (h->keyboard) /* a report only when something changes */
        usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0x0a /* SET_IDLE */, 0, in->number, NULL, 0);
    uint16_t len = h->report_len > ep->max_packet ? h->report_len : ep->max_packet;
    if (usb_enable_endpoint(d, ep) < 0 || usb_poll_endpoint(d, ep->address, len, report, h) < 0) {
        kfree(h);
        return false;
    }
    d->drv_data[i] = h;
    kprintf("usb: %s: %s%s%s\n", d->name, h->keyboard ? "keyboard" : "", h->keyboard && h->pointer ? " and " : "",
            h->pointer ? "pointer" : !h->keyboard ? "media keys" : "");
    return true;
}

static void disconnect(struct usb_device *d, int i) {
    struct hid *h = d->drv_data[i];
    if (!h)
        return;
    for (int k = 0; k < h->nkeys; k++) /* nothing left held down */
        evdev_push(&input_kbd, EV_KEY, h->keys[k], 0);
    if (h->nkeys)
        evdev_sync(&input_kbd);
    h->nkeys = 0;
    /* (h stays: the controller's ring still names it until the slot is gone) */
}

static struct usb_driver hid_driver = {.name = "hid", .probe = probe, .disconnect = disconnect};

void usb_hid_init(void);
void usb_hid_init(void) { usb_register_driver(&hid_driver); }
