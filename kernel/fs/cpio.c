#include <kernel/boot.h>
#include <kernel/lib.h>
#include <kernel/medium.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>

/* Unpacks a "newc" cpio archive (what `cpio -H newc` writes) into the root
 * file system. */

static uint32_t hex8(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v = (v << 4) | (uint32_t)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}


/* Hard links: newc gives every name of a file the same inode number, and
 * GNU cpio carries the data with only one of them (the last). The first
 * name creates the file; the others become links to it. */
#define LINK_BUCKETS 1024
struct linked {
    uint32_t ino;
    struct vnode *v;
    struct linked *next;
};

static struct vnode *linked_find(struct linked **tab, uint32_t ino) {
    for (struct linked *l = tab[ino % LINK_BUCKETS]; l; l = l->next)
        if (l->ino == ino)
            return l->v;
    return NULL;
}

static void linked_add(struct linked **tab, uint32_t ino, struct vnode *v) {
    struct linked *l = kmalloc(sizeof *l);
    if (!l)
        return;
    vnode_ref(v);
    *l = (struct linked){ino, v, tab[ino % LINK_BUCKETS]};
    tab[ino % LINK_BUCKETS] = l;
}

static void linked_free(struct linked **tab) {
    for (int b = 0; b < LINK_BUCKETS; b++)
        for (struct linked *l = tab[b], *n; l; l = n) {
            n = l->next;
            vnode_put(l->v);
            kfree(l);
        }
    kfree(tab);
}
/* The archive: whole in memory, or read through a window that moves along it. */
#define WINDOW (8u << 20)
struct src {
    const uint8_t *mem;
    uint64_t len;
    cpio_read_fn read;
    void *ctx;
    uint8_t *win;
    uint64_t win_off, win_len;
    bool failed;
};

/* `n` bytes (at most WINDOW / 2) at `off`, or NULL past the end or on a read error. */
static const uint8_t *src_at(struct src *s, uint64_t off, uint64_t n) {
    if (off + n > s->len || n > WINDOW / 2)
        return NULL;
    if (s->mem)
        return s->mem + off;
    if (off < s->win_off || off + n > s->win_off + s->win_len) {
        uint64_t start = ALIGN_DOWN(off, 2048);
        uint64_t want = s->len - start < WINDOW ? s->len - start : WINDOW;
        if (s->read(s->ctx, start, s->win, want) != 0) {
            s->failed = true;
            return NULL;
        }
        s->win_off = start, s->win_len = want;
    }
    return s->win + (off - s->win_off);
}

/* Writes the `size` bytes at `off` into `f`, in pieces. */
static bool src_write(struct src *s, struct file *f, uint64_t off, uint64_t size) {
    for (uint64_t done = 0; done < size;) {
        uint64_t n = size - done < WINDOW / 2 ? size - done : WINDOW / 2;
        const uint8_t *p = src_at(s, off + done, n);
        if (!p || file_write(f, p, n) != (int64_t)n)
            return false;
        done += n;
    }
    return true;
}

static bool unpack(struct src *s) {
    uint64_t off = 0;
    int files = 0;
    uint64_t t0 = time_monotonic_ns();
    char path[512];
    static char name_buf[4096]; /* (one archive at a time, at boot) */
    struct linked **links = kzalloc(sizeof *links * LINK_BUCKETS);
    while (off + 110 <= s->len) {
        const char *p = (const char *)src_at(s, off, 110);
        if (!p)
            break;
        if (memcmp(p, "070701", 6) && memcmp(p, "070702", 6))
            panic("initramfs: bad cpio header at offset %lu", off);
        uint32_t ino = hex8(p + 6), mode = hex8(p + 14), nlink = hex8(p + 38);
        uint32_t filesize = hex8(p + 54);
        uint32_t namesize = hex8(p + 94);
        if (namesize == 0 || namesize > sizeof name_buf)
            panic("initramfs: bad name at offset %lu", off);
        const char *nm = (const char *)src_at(s, off + 110, namesize);
        if (!nm)
            break;
        memcpy(name_buf, nm, namesize);
        name_buf[namesize - 1] = 0;
        const char *name = name_buf;
        uint64_t body = ALIGN_UP(off + 110 + namesize, 4);
        uint64_t next = ALIGN_UP(body + filesize, 4);
        if (body + filesize > s->len)
            panic("initramfs: truncated archive");
        if (!strcmp(name, "TRAILER!!!"))
            break;

        while (name[0] == '.' && name[1] == '/')
            name += 2;
        if (!strcmp(name, ".") || !name[0]) {
            off = next;
            continue;
        }
        ksnprintf(path, sizeof path, "/%s", name);
        uint32_t type = mode & S_IFMT, perm = mode & 07777;

        if (type == S_IFDIR) {
            int r = vfs_mkdir_p(path, perm);
            if (r < 0)
                panic("initramfs: mkdir %s failed (%d)", path, r);
        } else if (type == S_IFREG && nlink > 1 && links && linked_find(links, ino)) {
            /* another name of a file already made: link it, and its data may come with this one */
            struct vnode *v = linked_find(links, ino), *dir;
            char leaf[256];
            int r = vfs_lookup_parent(NULL, path, &dir, leaf, sizeof leaf);
            if (r == 0) {
                r = dir->ops->link ? dir->ops->link(dir, leaf, v) : -EPERM;
                vnode_put(dir);
            }
            if (r < 0)
                kprintf("initramfs: link %s failed (%d)\n", path, r);
            if (filesize) {
                struct file *f = file_from_vnode(v, O_WRONLY);
                if (!f || (v->ops->truncate && v->ops->truncate(v, 0) < 0) || !src_write(s, f, body, filesize))
                    panic("initramfs: write %s failed", path);
                file_put(f);
            }
        } else if (type == S_IFREG) {
            /* a later module replaces what an earlier one put here (a link too) */
            struct vnode *pdir;
            char pleaf[256];
            if (vfs_lookup_parent(NULL, path, &pdir, pleaf, sizeof pleaf) == 0) {
                struct vnode *old;
                if (pdir->ops->lookup && pdir->ops->lookup(pdir, pleaf, &old) == 0) {
                    bool link = old->type == VN_LNK;
                    vnode_put(old);
                    if (link && pdir->ops->unlink)
                        pdir->ops->unlink(pdir, pleaf, false);
                }
                vnode_put(pdir);
            }
            struct file *f;
            int r = vfs_open(NULL, path, O_WRONLY | O_CREAT | O_TRUNC, perm, &f);
            if (r < 0)
                panic("initramfs: create %s failed (%d)", path, r);
            f->vnode->mode = perm; /* not subject to umask */
            if (filesize && !src_write(s, f, body, filesize))
                panic("initramfs: write %s failed%s", path, s->failed ? " (the medium couldn't be read)" : "");
            if (nlink > 1 && links)
                linked_add(links, ino, f->vnode);
            file_put(f);
            files++;
        } else if (type == S_IFLNK) {
            char target[512];
            uint32_t n = filesize < sizeof target - 1 ? filesize : sizeof target - 1;
            const uint8_t *t = src_at(s, body, n);
            if (!t)
                break;
            memcpy(target, t, n);
            target[n] = 0;
            struct vnode *dir;
            char leaf[256];
            int r = vfs_lookup_parent(NULL, path, &dir, leaf, sizeof leaf);
            if (r == 0) {
                r = dir->ops->symlink(dir, leaf, target);
                if (r == -EEXIST && dir->ops->unlink && dir->ops->unlink(dir, leaf, false) == 0)
                    r = dir->ops->symlink(dir, leaf, target); /* a later module replaces it */
                vnode_put(dir);
            }
            if (r < 0)
                kprintf("initramfs: symlink %s failed (%d)\n", path, r);
        }
        off = next;
    }
    if (links)
        linked_free(links);
    kprintf("initramfs: %d files unpacked (%lu KiB%s) in %lu ms\n", files, s->len / 1024,
            s->mem ? "" : ", read as it went", (time_monotonic_ns() - t0) / 1000000);
    return !s->failed;
}

void initramfs_unpack(const void *data, size_t len) {
    struct src s = {.mem = data, .len = len};
    unpack(&s);
}

/* ---- an archive on the medium, by its index ----
 * The base system is 1.8 GB in some 15 000 files; unpacking it read all of it at boot (about a minute) and
 * kept all of it in memory, voices and models nobody had asked for yet. Its index (scripts/cpio-index.py,
 * built with the image and carried after the placeholder line the bootloader loads) says where each file's
 * bytes are in the archive: the files are made from that at once, and their pages read from the medium when
 * first used (drivers/medium.c, mm/pageobj.c), and given back when memory is short and nothing maps them.
 *
 * The index, little-endian: "OSIDX1\n\0", u64 the archive's size, u64 where its trailer's header is, u32 the
 * number of entries, u32 0; then each entry: u32 mode, u32 link group (0: none; the names of a file with
 * several are one file, made by the first), u64 size, u64 where the bytes are in the archive (regular files),
 * u16 the name's length, u16 the symlink target's length, u32 0, the name (no leading "./"), the target, and
 * padding to 4 bytes. */

struct idx_entry {
    uint32_t mode, group;
    uint64_t size, data;
    uint16_t name_len, target_len;
    uint32_t zero;
} __attribute__((packed));

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* The archive's trailer is where the index says it is: it is this archive's index. */
static bool trailer_there(const struct medium_file *f, uint64_t at) {
    uint8_t *b = kmalloc(2 * 2048);
    if (!b)
        return false;
    uint64_t a = ALIGN_DOWN(at, 2048);
    bool ok = at + 120 <= f->size && medium_read(f, a, b, at + 120 - a) == 0 && !memcmp(b + (at - a), "070701", 6) &&
              !memcmp(b + (at - a) + 110, "TRAILER!!!", 10);
    kfree(b);
    return ok;
}

bool initramfs_from_index(const void *index, uint64_t len, const struct medium_file *f) {
    const uint8_t *p = index;
    if (len < 32 || memcmp(p, "OSIDX1\n", 8))
        return false;
    uint64_t size = le64(p + 8), trailer = le64(p + 16);
    uint32_t count = (uint32_t)le64(p + 24);
    if (size != f->size || !trailer_there(f, trailer)) {
        kprintf("initramfs: the index is of another archive (%lu bytes, the medium's is %lu)\n", size, f->size);
        return false;
    }
    uint64_t t0 = time_monotonic_ns();
    /* check it all before making anything */
    uint64_t off = 32;
    for (uint32_t i = 0; i < count; i++) {
        if (off + sizeof(struct idx_entry) > len)
            return false;
        struct idx_entry e;
        memcpy(&e, p + off, sizeof e);
        uint64_t next = ALIGN_UP(off + sizeof e + e.name_len + e.target_len, 4);
        if (next > len || !e.name_len || e.name_len > 400 || e.target_len > 500 ||
            ((e.mode & S_IFMT) == S_IFREG && (e.data > size || e.size > size - e.data)))
            return false;
        off = next;
    }
    struct linked **links = kzalloc(sizeof *links * LINK_BUCKETS);
    char path[512], target[512];
    int files = 0, failed = 0;
    off = 32;
    for (uint32_t i = 0; i < count; i++) {
        struct idx_entry e;
        memcpy(&e, p + off, sizeof e);
        path[0] = '/';
        memcpy(path + 1, p + off + sizeof e, e.name_len);
        path[1 + e.name_len] = 0;
        memcpy(target, p + off + sizeof e + e.name_len, e.target_len);
        target[e.target_len] = 0;
        off = ALIGN_UP(off + sizeof e + e.name_len + e.target_len, 4);
        uint32_t type = e.mode & S_IFMT, perm = e.mode & 07777;
        int r = 0;
        if (type == S_IFDIR) {
            r = vfs_mkdir_p(path, perm);
        } else if (type == S_IFREG && e.group && links && linked_find(links, e.group)) {
            struct vnode *dir;
            char leaf[256];
            r = vfs_lookup_parent(NULL, path, &dir, leaf, sizeof leaf);
            if (r == 0) {
                r = dir->ops->link ? dir->ops->link(dir, leaf, linked_find(links, e.group)) : -EPERM;
                vnode_put(dir);
            }
        } else if (type == S_IFREG) {
            struct vnode *v = ramfs_source_file(perm, e.size, &medium_pages, (void *)f, e.data);
            r = v ? vfs_create_node(path, v) : -ENOMEM;
            if (r == -ENOENT) { /* (its directory wasn't in the archive) */
                char dir[512];
                size_t n = ksnprintf(dir, sizeof dir, "%s", path);
                while (n > 1 && dir[n - 1] != '/')
                    n--;
                if (n > 1) {
                    dir[n - 1] = 0;
                    vfs_mkdir_p(dir, 0755);
                    r = vfs_create_node(path, v);
                }
            }
            if (r == 0 && e.group && links)
                linked_add(links, e.group, v);
            if (v)
                vnode_put(v);
            files++;
        } else if (type == S_IFLNK) {
            struct vnode *dir;
            char leaf[256];
            r = vfs_lookup_parent(NULL, path, &dir, leaf, sizeof leaf);
            if (r == 0) {
                r = dir->ops->symlink(dir, leaf, target);
                vnode_put(dir);
            }
        }
        if (r < 0 && failed++ < 8)
            kprintf("initramfs: %s: %d\n", path, r);
    }
    if (links)
        linked_free(links);
    kprintf("initramfs: %d files from the index, %lu MiB left on the medium until used, in %lu ms\n", files,
            (unsigned long)(size >> 20), (time_monotonic_ns() - t0) / 1000000);
    return true;
}

bool initramfs_unpack_from(cpio_read_fn read, void *ctx, uint64_t len) {
    uint64_t phys = pmm_alloc(WINDOW / PAGE_SIZE);
    if (!phys)
        return false;
    struct src s = {.len = len, .read = read, .ctx = ctx, .win = phys_to_virt(phys)};
    bool ok = unpack(&s);
    pmm_free(phys, WINDOW / PAGE_SIZE);
    return ok;
}
