#include <kernel/input.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/platform.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>

/* Keyboard and mouse as Linux evdev devices (/dev/input/event0 keyboard,
 * /dev/input/event1 pointer), so programs read struct input_event exactly
 * as on Linux. The platform's input sources feed them through evdev_push.
 * As on Linux, every open has a queue of its own: each reader gets every
 * event (the boot screen and the desktop both see Esc, say), none takes it
 * from another. */

struct input_event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define QLEN 512

struct evclient { /* one open of the device */
    struct input_event q[QLEN];
    uint32_t head, tail;
    struct evclient *next;
};

struct evdev {
    struct evclient *clients;
    int opens;
    struct waitq wq;
    spinlock_t lock;
};

struct evdev input_kbd, input_mouse;

void evdev_push(struct evdev *d, uint16_t type, uint16_t code, int32_t value) {
    uint64_t ns = time_monotonic_ns();
    uint64_t f = spin_lock_irqsave(&d->lock);
    for (struct evclient *c = d->clients; c; c = c->next)
        if (c->head - c->tail < QLEN) {
            struct input_event *e = &c->q[c->head++ % QLEN];
            e->sec = (int64_t)(ns / 1000000000ULL);
            e->usec = (int64_t)(ns % 1000000000ULL / 1000);
            e->type = type;
            e->code = code;
            e->value = value;
        }
    spin_unlock_irqrestore(&d->lock, f);
}

void evdev_sync(struct evdev *d) {
    evdev_push(d, EV_SYN, 0, 0);
    waitq_wake_all(&d->wq);
}

bool evdev_is_open(struct evdev *d) { return d->opens > 0; }

static bool has_events(void *arg) {
    struct evclient *c = arg;
    return __atomic_load_n(&c->head, __ATOMIC_ACQUIRE) != c->tail;
}

static int64_t ev_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct evdev *d = v->data;
    struct evclient *c = f ? f->private : NULL;
    if (!c)
        return -EBADF;
    if (len < sizeof(struct input_event))
        return -EINVAL;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&d->lock);
        size_t n = 0;
        while (c->tail != c->head && (n + 1) * sizeof(struct input_event) <= len) {
            ((struct input_event *)buf)[n++] = c->q[c->tail++ % QLEN];
        }
        spin_unlock_irqrestore(&d->lock, fl);
        if (n)
            return (int64_t)(n * sizeof(struct input_event));
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (waitq_wait_cond(&d->wq, has_events, c) < 0)
            return -EINTR;
    }
}

static int ev_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    struct evdev *d = v->data;
    struct evclient *c = f ? f->private : NULL;
    if (wq)
        *wq = &d->wq;
    return c && has_events(c) ? POLLIN : 0;
}

static int ev_open(struct vnode *v, struct file *f) {
    struct evdev *d = v->data;
    struct evclient *c = kzalloc(sizeof *c);
    if (!c)
        return -ENOMEM;
    uint64_t fl = spin_lock_irqsave(&d->lock);
    c->next = d->clients;
    d->clients = c;
    d->opens++;
    spin_unlock_irqrestore(&d->lock, fl);
    if (f)
        f->private = c;
    return 0;
}

static void ev_close(struct vnode *v, struct file *f) {
    struct evdev *d = v->data;
    struct evclient *c = f ? f->private : NULL;
    uint64_t fl = spin_lock_irqsave(&d->lock);
    for (struct evclient **pp = &d->clients; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            break;
        }
    d->opens--;
    spin_unlock_irqrestore(&d->lock, fl);
    kfree(c);
}

static const struct vnode_ops ev_ops = {.read = ev_read, .poll = ev_poll, .open = ev_open, .close = ev_close};

static void add_dev(const char *path, struct evdev *d) {
    waitq_init(&d->wq);
    struct vnode *v = vnode_alloc(VN_CHR, 0660, &ev_ops);
    KASSERT(v);
    v->data = d;
    v->rdev = (13 << 8) | (d == &input_kbd ? 64 : 65);
    vfs_create_node(path, v);
    vnode_put(v);
}

void input_init(void) {
    vfs_mkdir_p("/dev/input", 0755);
    add_dev("/dev/input/event0", &input_kbd);
    add_dev("/dev/input/event1", &input_mouse);
    platform_input_init();
}
