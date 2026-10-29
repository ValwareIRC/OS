#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include "synth.h"

/* The engine behind /proc and /sys (see synth.h). */

#define SB_MAX (8u << 20) /* a runaway generator stops here */

static bool sb_grow(struct sbuf *b, size_t need) {
    if (b->oom)
        return false;
    if (b->len + need + 1 <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->len + need + 1)
        cap *= 2;
    char *nb = cap <= SB_MAX ? kmalloc(cap) : NULL;
    if (!nb) {
        b->oom = true;
        return false;
    }
    if (b->len)
        memcpy(nb, b->buf, b->len);
    kfree(b->buf);
    b->buf = nb;
    b->cap = cap;
    return true;
}

void sb_put(struct sbuf *b, const char *s, size_t n) {
    if (!sb_grow(b, n))
        return;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

void sb_printf(struct sbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    char small[256];
    int n = kvsnprintf(small, sizeof small, fmt, ap);
    if (n >= 0 && (size_t)n < sizeof small) {
        sb_put(b, small, (size_t)n);
    } else if (n > 0 && sb_grow(b, (size_t)n)) {
        kvsnprintf(b->buf + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
    va_end(ap);
}

void sb_pad(struct sbuf *b, size_t col) {
    size_t start = b->len;
    while (start && b->buf[start - 1] != '\n')
        start--;
    while (b->len - start < col && !b->oom)
        sb_put(b, " ", 1);
}

void sb_free(struct sbuf *b) {
    kfree(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* Stable inode numbers: the same node always gets the same one, so
 * getcwd's parent walk and `find` see a consistent tree. */
static uint64_t node_ino(const struct sent *e, const int id[3]) {
    uint64_t h = (uint64_t)(uintptr_t)e;
    for (int i = 0; i < 3; i++) {
        h ^= (uint64_t)(uint32_t)id[i] + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 31;
    }
    return (1ULL << 62) | (h & ((1ULL << 62) - 1));
}

static const struct vnode_ops synth_ops, synth_magic_ops;

/* A new vnode for entry `e` with ids `id`, under `parent` (referenced by
 * the child: generated directories exist only while something uses them). */
static struct vnode *make_node(const struct sent *e, const int id[3], struct vnode *parent) {
    struct snode *n = kmalloc(sizeof *n);
    struct vnode *v = vnode_alloc(e->type, e->mode, e->follow ? &synth_magic_ops : &synth_ops);
    if (v)
        v->uid = v->gid = 0; /* the kernel's, not the reader's */
    if (!n || !v) {
        kfree(n);
        kfree(v);
        return NULL;
    }
    n->ent = e;
    memcpy(n->id, id, sizeof n->id);
    v->data = n;
    v->ino = node_ino(e, id);
    v->nlink = e->type == VN_DIR ? 2 : 1;
    if (e->links)
        v->nlink = (uint32_t)e->links(n);
    v->parent = parent;
    if (parent)
        vnode_ref(parent);
    return v;
}

static int count_children(const struct sent *e) {
    int n = 0;
    while (e->children && e->children[n].name)
        n++;
    return n;
}

static int s_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct snode *n = dir->data;
    const struct sent *e = n->ent;
    for (const struct sent *c = e->children; c && c->name; c++) {
        if (!strcmp(c->name, name)) {
            *out = make_node(c, n->id, dir);
            return *out ? 0 : -ENOMEM;
        }
    }
    int id;
    if (e->dyn_ent && e->dyn_find && e->dyn_find(n, name, &id) == 0) {
        int ids[3];
        memcpy(ids, n->id, sizeof ids);
        ids[e->dyn_slot] = id;
        *out = make_node(e->dyn_ent, ids, dir);
        return *out ? 0 : -ENOMEM;
    }
    return -ENOENT;
}

static uint8_t dtype(enum vnode_type t) { return t == VN_DIR ? DT_DIR : t == VN_LNK ? DT_LNK : DT_REG; }

static int s_readdir(struct vnode *dir, uint64_t index, char *name, size_t name_size, uint8_t *type, uint64_t *ino) {
    struct snode *n = dir->data;
    const struct sent *e = n->ent;
    if (index < 2) {
        ksnprintf(name, name_size, index == 0 ? "." : "..");
        *type = DT_DIR;
        *ino = index == 0 ? dir->ino : (dir->parent ? dir->parent->ino : dir->ino);
        return 1;
    }
    uint64_t fixed = (uint64_t)count_children(e);
    if (index - 2 < fixed) {
        const struct sent *c = &e->children[index - 2];
        ksnprintf(name, name_size, "%s", c->name);
        *type = dtype(c->type);
        *ino = node_ino(c, n->id);
        return 1;
    }
    int id;
    if (!e->dyn_ent || !e->dyn_nth || e->dyn_nth(n, index - 2 - fixed, name, name_size, &id) <= 0)
        return 0;
    int ids[3];
    memcpy(ids, n->id, sizeof ids);
    ids[e->dyn_slot] = id;
    *type = dtype(e->dyn_ent->type);
    *ino = node_ino(e->dyn_ent, ids);
    return 1;
}

/* Contents are generated when a read starts at offset 0 and kept with the
 * open file for the reads that follow, so a file read in pieces is one
 * consistent snapshot (like Linux's seq_file), and rewinding refreshes it. */
static int64_t s_read(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off) {
    struct snode *n = v->data;
    if (!n->ent->show)
        return -EINVAL;
    struct sbuf fresh = {0}, *old = NULL;
    bool regen = !f || off == 0 || !f->private;
    if (regen) {
        int r = n->ent->show(n, &fresh);
        if (r < 0) {
            sb_free(&fresh);
            return r;
        }
        if (fresh.oom) {
            sb_free(&fresh);
            return -ENOMEM;
        }
    }
    int64_t got = 0;
    if (!f) {
        if (off < fresh.len) {
            got = (int64_t)(fresh.len - off < len ? fresh.len - off : len);
            memcpy(buf, fresh.buf + off, (size_t)got);
        }
        sb_free(&fresh);
        return got;
    }
    struct sbuf *keep = NULL;
    if (regen && !(keep = kmalloc(sizeof *keep))) {
        sb_free(&fresh);
        return -ENOMEM;
    }
    uint64_t fl = spin_lock_irqsave(&f->lock);
    if (regen) {
        *keep = fresh;
        old = f->private;
        f->private = keep;
    }
    struct sbuf *cur = f->private;
    if (off < cur->len) {
        got = (int64_t)(cur->len - off < len ? cur->len - off : len);
        memcpy(buf, cur->buf + off, (size_t)got);
    }
    spin_unlock_irqrestore(&f->lock, fl);
    if (old) {
        sb_free(old);
        kfree(old);
    }
    return got;
}

static int64_t s_write(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off) {
    (void)f;
    (void)off;
    struct snode *n = v->data;
    return n->ent->store ? n->ent->store(n, buf, len) : -EACCES;
}

static int s_truncate(struct vnode *v, uint64_t size) {
    (void)size;
    struct snode *n = v->data;
    return n->ent->store ? 0 : -EACCES; /* O_TRUNC on a writable file: nothing to drop */
}

static int s_readlink(struct vnode *v, char *buf, size_t size) {
    struct snode *n = v->data;
    if (!n->ent->link)
        return -EINVAL;
    char *tmp = kmalloc(4096);
    if (!tmp)
        return -ENOMEM;
    int r = n->ent->link(n, tmp, 4096);
    if (r >= 0) {
        if ((size_t)r > size)
            r = (int)size; /* readlink truncates silently */
        memcpy(buf, tmp, (size_t)r);
    }
    kfree(tmp);
    return r;
}

static int s_follow(struct vnode *v, struct path *out) {
    struct snode *n = v->data;
    return n->ent->follow(n, out);
}

static void s_close(struct vnode *v, struct file *f) {
    (void)v;
    struct sbuf *cache = f->private;
    if (cache) {
        sb_free(cache);
        kfree(cache);
        f->private = NULL;
    }
}

static void s_release(struct vnode *v) {
    kfree(v->data);
    if (v->parent && v->parent != v)
        vnode_put(v->parent);
}

/* Nodes are made on each lookup: the same entry with the same ids is the
 * same place (a mount on /proc/sys must be found again). */
static bool s_same(struct vnode *a, struct vnode *b) {
    const struct snode *x = a->data, *y = b->data;
    return x && y && x->ent == y->ent && !memcmp(x->id, y->id, sizeof x->id);
}

static const struct vnode_ops synth_ops = {
    .lookup = s_lookup, .readdir = s_readdir, .read = s_read, .write = s_write, .truncate = s_truncate,
    .readlink = s_readlink, .close = s_close, .release = s_release, .same = s_same,
};
static const struct vnode_ops synth_magic_ops = {
    .readlink = s_readlink, .follow = s_follow, .release = s_release,
};

struct vnode *synth_root(const struct sent *root) {
    static const int none[3];
    return make_node(root, none, NULL);
}
