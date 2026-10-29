#include <kernel/lib.h>
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
void initramfs_unpack(const void *data, size_t len) {
    const char *base = data, *p = data, *end = base + len;
    int files = 0;
    uint64_t t0 = time_monotonic_ns();
    char path[512];
    struct linked **links = kzalloc(sizeof *links * LINK_BUCKETS);
    while (p + 110 <= end) {
        if (memcmp(p, "070701", 6) && memcmp(p, "070702", 6))
            panic("initramfs: bad cpio header at offset %lu", (uint64_t)(p - base));
        uint32_t ino = hex8(p + 6), mode = hex8(p + 14), nlink = hex8(p + 38);
        uint32_t filesize = hex8(p + 54);
        uint32_t namesize = hex8(p + 94);
        const char *name = p + 110;
        const char *body = base + ALIGN_UP((uint64_t)(name + namesize - base), 4);
        const char *next = base + ALIGN_UP((uint64_t)(body + filesize - base), 4);
        if (body + filesize > end)
            panic("initramfs: truncated archive");
        if (!strcmp(name, "TRAILER!!!"))
            break;

        while (name[0] == '.' && name[1] == '/')
            name += 2;
        if (!strcmp(name, ".") || !name[0]) {
            p = next;
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
                if (!f || (v->ops->truncate && v->ops->truncate(v, 0) < 0) || file_write(f, body, filesize) != (int64_t)filesize)
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
            if (filesize && file_write(f, body, filesize) != (int64_t)filesize)
                panic("initramfs: write %s failed", path);
            if (nlink > 1 && links)
                linked_add(links, ino, f->vnode);
            file_put(f);
            files++;
        } else if (type == S_IFLNK) {
            char target[512];
            uint32_t n = filesize < sizeof target - 1 ? filesize : sizeof target - 1;
            memcpy(target, body, n);
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
        p = next;
    }
    if (links)
        linked_free(links);
    kprintf("initramfs: %d files unpacked (%lu KiB) in %lu ms\n", files, (uint64_t)len / 1024,
            (time_monotonic_ns() - t0) / 1000000);
}
