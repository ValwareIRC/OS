#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* Pseudo-terminals, the Linux way: opening /dev/ptmx makes a new pair and
 * returns its master end; the slave appears as /dev/pts/N. Terminal windows
 * hold the master; the shell inside reads and writes the slave, which has the
 * same line discipline as the console (canonical mode, echo, ^C ^Z ^D ...). */

#define PTY_MAX 64
#define RING 8192

#define TIOCGPTN 0x80045430
#define TIOCSPTLCK 0x40045431
#define TIOCNOTTY 0x5422
#define TIOCGSID 0x5429
#define TIOCOUTQ 0x5411

struct ring {
    char buf[RING];
    uint64_t head, tail;
};

struct pty {
    int index;
    spinlock_t lock;
    struct waitq wq; /* both directions: readers and writers of either end */
    struct ring in;  /* master -> slave, after the line discipline */
    uint64_t line_end;
    bool eof_pending;
    struct ring out; /* slave -> master: program output and echo */
    struct linux_termios termios;
    struct linux_winsize winsize;
    int fg_pgrp, sid;
    bool master_open, locked, slave_opened;
    int slave_files;
    struct vnode *slave;
};

static struct pty *ptys[PTY_MAX];
static spinlock_t ptys_lock = SPINLOCK_INIT;

int proc_kill_pgrp(int pgid, int sig);

static size_t ring_used(const struct ring *r) { return (size_t)(r->head - r->tail); }
static size_t ring_free(const struct ring *r) { return RING - ring_used(r); }
static void ring_put(struct ring *r, char c) { r->buf[r->head++ % RING] = c; }

/* Output toward the master, with OPOST/ONLCR. Lock held. */
static void out_char(struct pty *p, char c) {
    if (c == '\n' && (p->termios.c_oflag & TC_OPOST) && (p->termios.c_oflag & TC_ONLCR)) {
        if (ring_free(&p->out) >= 2) {
            ring_put(&p->out, '\r');
            ring_put(&p->out, '\n');
        }
        return;
    }
    if (ring_free(&p->out))
        ring_put(&p->out, c);
}

static void echo(struct pty *p, const char *s, size_t n) {
    if (!(p->termios.c_lflag & TC_ECHO))
        return;
    for (size_t i = 0; i < n; i++)
        out_char(p, s[i]);
}

/* One byte typed at the master. Lock held; returns a signal to send to the
 * foreground group (after unlocking), or 0. */
static int ldisc_input(struct pty *p, char c) {
    struct linux_termios *t = &p->termios;
    if (c == '\r' && (t->c_iflag & TC_ICRNL))
        c = '\n';
    if (t->c_lflag & TC_ISIG) {
        int sig = c == (char)t->c_cc[VINTR]  ? SIGINT
                  : c == (char)t->c_cc[VQUIT] ? SIGQUIT
                  : c == (char)t->c_cc[VSUSP] ? SIGTSTP
                                              : 0;
        if (sig) {
            char e[3] = {'^', (char)(c + '@'), '\n'};
            echo(p, e, 3);
            p->in.head = p->line_end; /* drop the partial line */
            return sig;
        }
    }
    if (!(t->c_lflag & TC_ICANON)) {
        if (ring_free(&p->in)) {
            ring_put(&p->in, c);
            p->line_end = p->in.head;
            echo(p, &c, 1);
        }
    } else if (c == (char)t->c_cc[VERASE] || c == '\b') {
        if (p->in.head > p->line_end) {
            p->in.head--;
            echo(p, "\b \b", 3);
        }
    } else if (c == (char)t->c_cc[VKILL]) {
        while (p->in.head > p->line_end) {
            p->in.head--;
            echo(p, "\b \b", 3);
        }
    } else if (c == (char)t->c_cc[VEOF]) {
        if (p->in.head == p->line_end)
            p->eof_pending = true;
        p->line_end = p->in.head;
    } else if (ring_free(&p->in) > 1) {
        ring_put(&p->in, c);
        echo(p, &c, 1);
        if (c == '\n')
            p->line_end = p->in.head;
    }
    return 0;
}

static void signal_fg(struct pty *p, int sig) {
    if (sig && p->fg_pgrp > 0)
        proc_kill_pgrp(p->fg_pgrp, sig);
}

/* ---- wait conditions ---- */

static bool slave_readable(void *arg) {
    struct pty *p = arg;
    if (!p->master_open)
        return true;
    if (p->termios.c_lflag & TC_ICANON)
        return p->line_end != p->in.tail || p->eof_pending;
    return p->in.head != p->in.tail;
}
static bool master_readable(void *arg) {
    struct pty *p = arg;
    return ring_used(&p->out) || (p->slave_opened && p->slave_files == 0);
}
static bool slave_writable(void *arg) {
    struct pty *p = arg;
    return !p->master_open || ring_free(&p->out) >= 2;
}
static bool master_writable(void *arg) {
    struct pty *p = arg;
    return ring_free(&p->in) > 1 || !(p->termios.c_lflag & TC_ICANON);
}

/* ---- shared ioctls ---- */

static int64_t pty_ioctl(struct pty *p, bool master, uint64_t req, uint64_t arg) {
    struct process *cur = proc_current();
    switch (req) {
    case TCGETS:
        return copy_to_user(arg, &p->termios, sizeof p->termios);
    case TCGETS2: {
        struct linux_termios2 t2 = {p->termios, 38400, 38400};
        return copy_to_user(arg, &t2, sizeof t2);
    }
    case TCSETS2:
    case TCSETSW2:
    case TCSETSF2:
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct linux_termios t;
        int r = copy_from_user(&t, arg, sizeof t); /* termios2 starts with the same fields */
        if (req == TCSETSF2)
            req = TCSETSF;
        if (r)
            return r;
        uint64_t fl = spin_lock_irqsave(&p->lock);
        bool was_canon = p->termios.c_lflag & TC_ICANON;
        p->termios = t;
        if (was_canon && !(t.c_lflag & TC_ICANON))
            p->line_end = p->in.head;
        if (req == TCSETSF)
            p->in.tail = p->line_end = p->in.head;
        spin_unlock_irqrestore(&p->lock, fl);
        waitq_wake_all(&p->wq);
        return 0;
    }
    case TIOCGWINSZ:
        return copy_to_user(arg, &p->winsize, sizeof p->winsize);
    case TIOCSWINSZ: {
        struct linux_winsize w;
        int r = copy_from_user(&w, arg, sizeof w);
        if (r)
            return r;
        bool changed = memcmp(&w, &p->winsize, sizeof w) != 0;
        p->winsize = w;
        if (changed)
            signal_fg(p, SIGWINCH);
        return 0;
    }
    case TIOCGPGRP:
        return copy_to_user(arg, &p->fg_pgrp, sizeof p->fg_pgrp);
    case TIOCSPGRP:
        return copy_from_user(&p->fg_pgrp, arg, sizeof p->fg_pgrp);
    case TIOCGSID:
        return copy_to_user(arg, &p->sid, sizeof p->sid);
    case TIOCSCTTY:
        if (master)
            return -EINVAL;
        p->sid = cur->sid;
        p->fg_pgrp = cur->pgid;
        cur->ctty = p->index + 1;
        return 0;
    case TIOCNOTTY:
        if (cur->ctty == p->index + 1)
            cur->ctty = -1;
        return 0;
    case FIONREAD: {
        int n = master ? (int)ring_used(&p->out) : (int)(p->line_end - p->in.tail);
        return copy_to_user(arg, &n, sizeof n);
    }
    case TIOCOUTQ: {
        int n = master ? 0 : (int)ring_used(&p->out);
        return copy_to_user(arg, &n, sizeof n);
    }
    case TIOCGPTN:
        if (!master)
            return -ENOTTY;
        return copy_to_user(arg, &p->index, sizeof p->index);
    case TIOCSPTLCK: {
        if (!master)
            return -ENOTTY;
        int lock;
        int r = copy_from_user(&lock, arg, sizeof lock);
        if (!r)
            p->locked = lock != 0;
        return r;
    }
    }
    return -ENOTTY;
}

/* ---- the slave end: /dev/pts/N ---- */

static int64_t slave_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)off;
    struct pty *p = v->data;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&p->lock);
        bool canon = p->termios.c_lflag & TC_ICANON;
        uint64_t limit = canon ? p->line_end : p->in.head;
        if (limit != p->in.tail) {
            size_t n = 0;
            while (n < len && p->in.tail < limit) {
                char c = p->in.buf[p->in.tail++ % RING];
                ((char *)buf)[n++] = c;
                if (canon && c == '\n')
                    break;
            }
            spin_unlock_irqrestore(&p->lock, fl);
            waitq_wake_all(&p->wq);
            return (int64_t)n;
        }
        if (canon && p->eof_pending) {
            p->eof_pending = false;
            spin_unlock_irqrestore(&p->lock, fl);
            return 0;
        }
        bool hung = !p->master_open;
        spin_unlock_irqrestore(&p->lock, fl);
        if (hung)
            return 0;
        if (f && (f->flags & O_NONBLOCK))
            return -EAGAIN;
        if (waitq_wait_cond(&p->wq, slave_readable, p) < 0)
            return -EINTR;
    }
}

static int64_t slave_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)off;
    struct pty *p = v->data;
    size_t done = 0;
    while (done < len) {
        uint64_t fl = spin_lock_irqsave(&p->lock);
        if (!p->master_open) {
            spin_unlock_irqrestore(&p->lock, fl);
            return done ? (int64_t)done : -EIO;
        }
        while (done < len && ring_free(&p->out) >= 2)
            out_char(p, ((const char *)buf)[done++]);
        spin_unlock_irqrestore(&p->lock, fl);
        waitq_wake_all(&p->wq);
        if (done < len) {
            if (f && (f->flags & O_NONBLOCK))
                return done ? (int64_t)done : -EAGAIN;
            if (waitq_wait_cond(&p->wq, slave_writable, p) < 0)
                return done ? (int64_t)done : -EINTR;
        }
    }
    return (int64_t)done;
}

static int slave_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)f;
    struct pty *p = v->data;
    if (wq)
        *wq = &p->wq;
    int ev = 0;
    if (slave_readable(p))
        ev |= POLLIN;
    if (!p->master_open)
        ev |= POLLHUP;
    else if (ring_free(&p->out) >= 2)
        ev |= POLLOUT;
    return ev;
}

static int64_t slave_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)f;
    return pty_ioctl(v->data, false, req, arg);
}

static int slave_open(struct vnode *v, struct file *f) {
    struct pty *p = v->data;
    if (!p->master_open || p->locked)
        return -EIO;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    p->slave_files++;
    p->slave_opened = true;
    spin_unlock_irqrestore(&p->lock, fl);
    /* A session leader without a terminal acquires this one. */
    struct process *cur = proc_current();
    if (!(f->flags & O_NOCTTY) && cur->sid == cur->pid && cur->ctty == -1 && p->sid == 0) {
        cur->ctty = p->index + 1;
        p->sid = cur->sid;
        p->fg_pgrp = cur->pgid;
    }
    return 0;
}

static void slave_close(struct vnode *v, struct file *f) {
    (void)f;
    struct pty *p = v->data;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    p->slave_files--;
    spin_unlock_irqrestore(&p->lock, fl);
    waitq_wake_all(&p->wq); /* the master sees the hangup */
}

static void slave_release(struct vnode *v) {
    /* Unlinked by the master's close and no files left: the pair goes. */
    struct pty *p = v->data;
    uint64_t fl = spin_lock_irqsave(&ptys_lock);
    if (ptys[p->index] == p)
        ptys[p->index] = NULL;
    spin_unlock_irqrestore(&ptys_lock, fl);
    kfree(p);
}

static const struct vnode_ops slave_ops = {
    .read = slave_read, .write = slave_write, .poll = slave_poll, .ioctl = slave_ioctl,
    .open = slave_open, .close = slave_close, .release = slave_release,
};

/* ---- the master end: each open of /dev/ptmx ---- */

static int64_t master_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    (void)v, (void)off;
    struct pty *p = f->private;
    for (;;) {
        uint64_t fl = spin_lock_irqsave(&p->lock);
        size_t n = 0;
        while (n < len && ring_used(&p->out))
            ((char *)buf)[n++] = p->out.buf[p->out.tail++ % RING];
        bool hung = p->slave_opened && p->slave_files == 0;
        spin_unlock_irqrestore(&p->lock, fl);
        if (n) {
            waitq_wake_all(&p->wq);
            return (int64_t)n;
        }
        if (hung)
            return -EIO; /* as on Linux: every slave closed */
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        if (waitq_wait_cond(&p->wq, master_readable, p) < 0)
            return -EINTR;
    }
}

static int64_t master_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)v, (void)off;
    struct pty *p = f->private;
    size_t done = 0;
    while (done < len) {
        int sig = 0;
        uint64_t fl = spin_lock_irqsave(&p->lock);
        while (done < len && !sig && master_writable(p))
            sig = ldisc_input(p, ((const char *)buf)[done++]);
        spin_unlock_irqrestore(&p->lock, fl);
        waitq_wake_all(&p->wq);
        signal_fg(p, sig);
        if (done < len && !sig) {
            if (f->flags & O_NONBLOCK)
                return done ? (int64_t)done : -EAGAIN;
            if (waitq_wait_cond(&p->wq, master_writable, p) < 0)
                return done ? (int64_t)done : -EINTR;
        }
    }
    return (int64_t)done;
}

static int master_poll(struct vnode *v, struct file *f, struct waitq **wq) {
    (void)v;
    struct pty *p = f->private;
    if (!p)
        return POLLERR;
    if (wq)
        *wq = &p->wq;
    int ev = 0;
    if (ring_used(&p->out))
        ev |= POLLIN;
    if (p->slave_opened && p->slave_files == 0)
        ev |= POLLHUP;
    if (master_writable(p))
        ev |= POLLOUT;
    return ev;
}

static int64_t master_ioctl(struct vnode *v, struct file *f, uint64_t req, uint64_t arg) {
    (void)v;
    return pty_ioctl(f->private, true, req, arg);
}

static int master_open(struct vnode *v, struct file *f) {
    (void)v;
    struct pty *p = kzalloc(sizeof *p);
    if (!p)
        return -ENOMEM;
    uint64_t fl = spin_lock_irqsave(&ptys_lock);
    int idx = -1;
    for (int i = 0; i < PTY_MAX; i++)
        if (!ptys[i]) {
            idx = i;
            ptys[i] = p;
            break;
        }
    spin_unlock_irqrestore(&ptys_lock, fl);
    if (idx < 0) {
        kfree(p);
        return -ENOSPC;
    }
    p->index = idx;
    p->master_open = true;
    p->locked = true; /* until unlockpt() */
    waitq_init(&p->wq);
    struct linux_termios *t = &p->termios;
    t->c_iflag = TC_ICRNL;
    t->c_oflag = TC_OPOST | TC_ONLCR;
    t->c_cflag = 0x00bf;
    t->c_lflag = TC_ISIG | TC_ICANON | TC_ECHO | TC_ECHOE | TC_ECHOK | TC_ECHOCTL | TC_IEXTEN;
    t->c_cc[VINTR] = 3;
    t->c_cc[VQUIT] = 0x1c;
    t->c_cc[VERASE] = 0x7f;
    t->c_cc[VKILL] = 0x15;
    t->c_cc[VEOF] = 4;
    t->c_cc[VMIN] = 1;
    t->c_cc[VSUSP] = 0x1a;
    p->winsize.ws_row = 24;
    p->winsize.ws_col = 80;

    struct vnode *s = vnode_alloc(VN_CHR, 0620, &slave_ops);
    if (!s) {
        ptys[idx] = NULL;
        kfree(p);
        return -ENOMEM;
    }
    s->rdev = (136 << 8) | (uint32_t)idx;
    s->data = p;
    p->slave = s;
    char path[32];
    ksnprintf(path, sizeof path, "/dev/pts/%d", idx);
    vfs_create_node(path, s);
    vnode_put(s); /* the directory entry keeps it */
    f->private = p;
    return 0;
}

static void master_close(struct vnode *v, struct file *f) {
    (void)v;
    struct pty *p = f->private;
    if (!p)
        return;
    uint64_t fl = spin_lock_irqsave(&p->lock);
    p->master_open = false;
    int pg = p->fg_pgrp;
    spin_unlock_irqrestore(&p->lock, fl);
    waitq_wake_all(&p->wq);
    if (pg > 0)
        proc_kill_pgrp(pg, SIGHUP);
    /* Remove /dev/pts/N; the pair is freed when its last slave file goes. */
    char path[32], name[256];
    ksnprintf(path, sizeof path, "/dev/pts/%d", p->index);
    struct vnode *dir;
    if (vfs_lookup_parent(NULL, path, &dir, name, sizeof name) == 0) {
        if (dir->ops->unlink)
            dir->ops->unlink(dir, name, false);
        vnode_put(dir);
    }
}

static const struct vnode_ops ptmx_ops = {
    .read = master_read, .write = master_write, .poll = master_poll, .ioctl = master_ioctl,
    .open = master_open, .close = master_close,
};

/* For /dev/tty: the caller's controlling pty (referenced), or NULL. */
struct vnode *pty_slave_vnode(int index) {
    struct vnode *v = NULL;
    uint64_t fl = spin_lock_irqsave(&ptys_lock);
    if (index >= 0 && index < PTY_MAX && ptys[index] && ptys[index]->master_open) {
        v = ptys[index]->slave;
        vnode_ref(v);
    }
    spin_unlock_irqrestore(&ptys_lock, fl);
    return v;
}

void pty_init(void) {
    vfs_mkdir_p("/dev/pts", 0755);
    struct vnode *v = vnode_alloc(VN_CHR, 0666, &ptmx_ops);
    KASSERT(v);
    v->rdev = (5 << 8) | 2;
    vfs_create_node("/dev/ptmx", v);
    /* and inside devpts, as on Linux: sandboxes (bubblewrap, so every Flatpak
     * app) mount devpts and make their /dev/ptmx a link to pts/ptmx */
    vfs_create_node("/dev/pts/ptmx", v);
    vnode_put(v);
}
