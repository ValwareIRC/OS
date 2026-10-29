#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

#define PIPE_SIZE 65536

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint64_t head, tail; /* bytes written / read, ever */
    int readers, writers;
    struct waitq wq; /* readers and writers both wait here */
    spinlock_t lock;
};

static bool readable(void *arg) {
    struct pipe *p = arg;
    return p->head != p->tail || p->writers == 0;
}

static bool writable(void *arg) {
    struct pipe *p = arg;
    return p->head - p->tail < PIPE_SIZE || p->readers == 0;
}

static int64_t pipe_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct pipe *p = v->data;
    if (!len)
        return 0;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&p->lock);
        uint64_t avail = p->head - p->tail;
        if (avail) {
            size_t n = avail < len ? avail : len;
            for (size_t i = 0; i < n; i++)
                ((uint8_t *)buf)[i] = p->buf[(p->tail + i) % PIPE_SIZE];
            p->tail += n;
            spin_unlock_irqrestore(&p->lock, fl);
            waitq_wake_all(&p->wq);
            return (int64_t)n;
        }
        bool eof = p->writers == 0;
        spin_unlock_irqrestore(&p->lock, fl);
        if (eof)
            return 0;
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&p->wq, readable, p) < 0)
            return -EINTR;
    }
}

static int64_t pipe_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)off;
    struct pipe *p = v->data;
    size_t done = 0;
    while (done < len) {
        uint64_t fl = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, fl);
            proc_kill(proc_current(), SIGPIPE);
            return done ? (int64_t)done : -EPIPE;
        }
        uint64_t space = PIPE_SIZE - (p->head - p->tail);
        size_t n = space < len - done ? space : len - done;
        for (size_t i = 0; i < n; i++)
            p->buf[(p->head + i) % PIPE_SIZE] = ((const uint8_t *)buf)[done + i];
        p->head += n;
        done += n;
        spin_unlock_irqrestore(&p->lock, fl);
        if (n)
            waitq_wake_all(&p->wq);
        if (done < len) {
            if (f && (f->flags & O_NONBLOCK))
                return done ? (int64_t)done : -EAGAIN;
            if (waitq_wait_cond(&p->wq, writable, p) < 0)
                return done ? (int64_t)done : -EINTR;
        }
    }
    return (int64_t)done;
}

static int pipe_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    struct pipe *p = v->data;
    int ev = 0;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        if (p->head != p->tail)
            ev |= POLLIN;
        if (p->writers == 0)
            ev |= POLLHUP;
    } else {
        if (p->head - p->tail < PIPE_SIZE)
            ev |= POLLOUT;
        if (p->readers == 0)
            ev |= POLLERR;
    }
    spin_unlock_irqrestore(&p->lock, fl);
    if (wq)
        *wq = &p->wq;
    return ev;
}

static void pipe_close(struct vnode *v, struct file *f) {
    struct pipe *p = v->data;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        p->readers--;
    else
        p->writers--;
    spin_unlock_irqrestore(&p->lock, fl);
    waitq_wake_all(&p->wq);
}

static void pipe_release(struct vnode *v) { kfree(v->data); }

/* Reopened through /proc/<pid>/fd/N (bash's <(...) is /dev/fd/63): one
 * more reader or writer, which pipe_close takes away again. */
static int pipe_open(struct vnode *v, struct file *f) {
    struct pipe *p = v->data;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        p->readers++;
    else
        p->writers++;
    spin_unlock_irqrestore(&p->lock, fl);
    return 0;
}

static int64_t pipe_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)f;
    struct pipe *p = v->data;
    if (req == FIONREAD) {
        int n = (int)(p->head - p->tail);
        return copy_to_user(arg, &n, sizeof n);
    }
    return -ENOTTY;
}

static const struct vnode_ops pipe_ops = {
    .read = pipe_read, .write = pipe_write, .poll = pipe_poll,
    .close = pipe_close, .release = pipe_release, .ioctl = pipe_ioctl, .open = pipe_open,
};

int pipe_create(struct file **read_end, struct file **write_end) {
    struct pipe *p = kzalloc(sizeof *p);
    struct vnode *v = vnode_alloc(VN_FIFO, 0600, &pipe_ops);
    if (!p || !v) {
        kfree(p);
        kfree(v);
        return -ENOMEM;
    }
    waitq_init(&p->wq);
    p->readers = p->writers = 1;
    v->data = p;
    *read_end = file_from_vnode(v, O_RDONLY);
    *write_end = file_from_vnode(v, O_WRONLY);
    vnode_put(v);
    if (!*read_end || !*write_end) {
        if (*read_end)
            file_put(*read_end); /* closing one end is harmless: no users yet */
        if (*write_end)
            file_put(*write_end);
        return -ENOMEM;
    }
    return 0;
}
