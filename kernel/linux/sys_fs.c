#include <kernel/blkdev.h>
#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/syscall.h>

#define BOUNCE 65536
#define NAME_MAX 255

/* ---- descriptors ---- */

struct file *fd_get(int fd) {
    struct fdtable *t = proc_current()->fds;
    if (fd < 0 || fd >= MAX_FDS || !t)
        return NULL;
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct file *file = t->fd[fd];
    if (file)
        file_ref(file);
    spin_unlock_irqrestore(&t->lock, f);
    return file;
}

int fd_install(struct file *file, int min_fd, bool cloexec) {
    struct fdtable *t = proc_current()->fds;
    uint64_t f = spin_lock_irqsave(&t->lock);
    for (int i = min_fd < 0 ? 0 : min_fd; i < MAX_FDS; i++) {
        if (!t->fd[i]) {
            t->fd[i] = file;
            t->cloexec[i] = cloexec;
            spin_unlock_irqrestore(&t->lock, f);
            return i;
        }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return -EMFILE;
}

int fd_close(int fd) {
    struct fdtable *t = proc_current()->fds;
    if (fd < 0 || fd >= MAX_FDS)
        return -EBADF;
    uint64_t f = spin_lock_irqsave(&t->lock);
    struct file *file = t->fd[fd];
    t->fd[fd] = NULL;
    t->cloexec[fd] = 0;
    spin_unlock_irqrestore(&t->lock, f);
    if (!file)
        return -EBADF;
    void locks_closed_by(struct process *p, struct vnode *vn);
    locks_closed_by(proc_current(), file->vnode); /* POSIX: any close lets go of the process's locks */
    file_put(file);
    return 0;
}

int vnode_install_fd(struct vnode *v, uint32_t flags, bool cloexec) {
    struct file *f = file_from_vnode(v, flags);
    vnode_put(v);
    if (!f)
        return -ENOMEM;
    int fd = fd_install(f, 0, cloexec);
    if (fd < 0)
        file_put(f);
    return fd;
}

int64_t user_path(uint64_t uptr, char **out) {
    char *buf = kmalloc(PATH_MAX);
    if (!buf)
        return -ENOMEM;
    int64_t n = strncpy_from_user(buf, uptr, PATH_MAX);
    if (n < 0) {
        kfree(buf);
        return n;
    }
    *out = buf;
    return n;
}

/* Where an open file is: its mount and vnode (referenced). */
static void file_place(struct file *f, struct path *out) {
    if (f->mnt) {
        out->mnt = mount_get(f->mnt);
        out->v = f->vnode;
        vnode_ref(out->v);
    } else {
        path_for_vnode(f->vnode, out);
    }
}

int at_dir(int dirfd, const char *path, struct path *out) {
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        fs_context(NULL, out);
        return 0;
    }
    struct file *f = fd_get(dirfd);
    if (!f)
        return -EBADF;
    if (f->vnode->type != VN_DIR) {
        file_put(f);
        return -ENOTDIR;
    }
    file_place(f, out);
    file_put(f);
    return 0;
}

/* The directory a new or removed name is in, on a writable mount. */
static int parent_at(int dirfd, const char *path, struct path *dir, char *name, size_t size) {
    struct path base;
    int r = at_dir(dirfd, path, &base);
    if (r < 0)
        return r;
    r = vfs_lookup_parent_path(&base, path, dir, name, size);
    path_put(&base);
    if (r == 0 && (r = path_may_write(dir)) < 0)
        path_put(dir);
    return r;
}

/* Runs `fn` with a kernel copy of the path and its starting directory. */
#define WITH_PATH_AT(dirfd, upath, body)                                                                 \
    ({                                                                                                   \
        char *path;                                                                                      \
        int64_t _r = user_path((upath), &path);                                                          \
        if (_r >= 0) {                                                                                   \
            struct path base;                                                                            \
            _r = at_dir((int)(dirfd), path, &base);                                                      \
            if (_r == 0) {                                                                               \
                _r = (body);                                                                             \
                path_put(&base);                                                                         \
            }                                                                                            \
            kfree(path);                                                                                 \
        }                                                                                                \
        _r;                                                                                              \
    })

/* ---- read / write ---- */

static int64_t do_read(struct file *f, uint64_t ubuf, uint64_t len, int64_t off) {
    if (len == 0)
        return 0;
    size_t chunk = len < BOUNCE ? len : BOUNCE;
    uint8_t *kb = kmalloc(chunk);
    if (!kb)
        return -ENOMEM;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? len - (uint64_t)total : chunk;
        int64_t n = off < 0 ? file_read(f, kb, want) : file_pread(f, kb, want, (uint64_t)off + (uint64_t)total);
        if (n <= 0) {
            if (total == 0)
                total = n;
            break;
        }
        if (copy_to_user(ubuf + (uint64_t)total, kb, (size_t)n) < 0) {
            total = total ? total : -EFAULT;
            break;
        }
        total += n;
        /* Terminals and pipes: return what we have rather than block again. */
        if ((size_t)n < want || !vnode_seekable(f->vnode))
            break;
    }
    kfree(kb);
    return total;
}

static int64_t do_write(struct file *f, uint64_t ubuf, uint64_t len, int64_t off) {
    if (len == 0)
        return 0;
    size_t chunk = len < BOUNCE ? len : BOUNCE;
    uint8_t *kb = kmalloc(chunk);
    if (!kb)
        return -ENOMEM;
    int64_t total = 0;
    while ((uint64_t)total < len) {
        size_t want = len - (uint64_t)total < chunk ? len - (uint64_t)total : chunk;
        if (copy_from_user(kb, ubuf + (uint64_t)total, want) < 0) {
            total = total ? total : -EFAULT;
            break;
        }
        int64_t n;
        if (off < 0) {
            n = file_write(f, kb, want);
        } else {
            struct vnode *v = f->vnode;
            n = v->ops->write ? v->ops->write(v, f, kb, want, (uint64_t)off + (uint64_t)total) : -EINVAL;
        }
        if (n <= 0) {
            if (total == 0)
                total = n;
            break;
        }
        total += n;
        if ((size_t)n < want)
            break;
    }
    kfree(kb);
    return total;
}

SYSCALL(read) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t n = do_read(f, a2, a3, -1);
    file_put(f);
    return n;
}

SYSCALL(write) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t n = do_write(f, a2, a3, -1);
    file_put(f);
    return n;
}

SYSCALL(pread64) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t n = vnode_seekable(f->vnode) ? do_read(f, a2, a3, (int64_t)a4) : -ESPIPE;
    file_put(f);
    return n;
}

SYSCALL(pwrite64) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t n = vnode_seekable(f->vnode) ? do_write(f, a2, a3, (int64_t)a4) : -ESPIPE;
    file_put(f);
    return n;
}

static int64_t do_iov(uint64_t fd, uint64_t uiov, uint64_t cnt, bool write) {
    if (cnt > 1024)
        return -EINVAL;
    struct file *f = fd_get((int)fd);
    if (!f)
        return -EBADF;
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        struct linux_iovec iov;
        if (copy_from_user(&iov, uiov + i * sizeof iov, sizeof iov) < 0) {
            total = total ? total : -EFAULT;
            break;
        }
        int64_t n = write ? do_write(f, iov.iov_base, iov.iov_len, -1) : do_read(f, iov.iov_base, iov.iov_len, -1);
        if (n < 0) {
            total = total ? total : n;
            break;
        }
        total += n;
        if ((uint64_t)n < iov.iov_len)
            break;
    }
    file_put(f);
    return total;
}

SYSCALL(readv) {
    UNUSED_ARGS();
    return do_iov(a1, a2, a3, false);
}

SYSCALL(writev) {
    UNUSED_ARGS();
    return do_iov(a1, a2, a3, true);
}

SYSCALL(lseek) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t r;
    if (f->vnode->type == VN_FIFO || f->vnode->type == VN_CHR || f->vnode->type == VN_SOCK ||
        f->vnode->type == VN_ANON) {
        r = -ESPIPE;
    } else {
        int64_t off = (int64_t)a2, base = a3 == SEEK_SET ? 0 : a3 == SEEK_CUR ? (int64_t)f->offset
                                                     : a3 == SEEK_END ? (int64_t)f->vnode->size : -1;
        if (base < 0 && a3 != SEEK_SET)
            r = -EINVAL;
        else if (base + off < 0)
            r = -EINVAL;
        else
            r = (int64_t)(f->offset = (uint64_t)(base + off));
    }
    file_put(f);
    return r;
}

/* ---- open / close ---- */

static int64_t do_openat(int dirfd, uint64_t upath, uint64_t flags, uint64_t mode) {
    struct file *f = NULL;
    int64_t r = WITH_PATH_AT(dirfd, upath, vfs_open(&base, path, (uint32_t)flags, (uint32_t)mode, &f));
    if (r < 0)
        return r;
    r = fd_install(f, 0, flags & O_CLOEXEC);
    if (r < 0)
        file_put(f);
    return r;
}

SYSCALL(open) {
    UNUSED_ARGS();
    return do_openat(AT_FDCWD, a1, a2, a3);
}

SYSCALL(openat) {
    UNUSED_ARGS();
    return do_openat((int)a1, a2, a3, a4);
}

SYSCALL(creat) {
    UNUSED_ARGS();
    return do_openat(AT_FDCWD, a1, O_CREAT | O_WRONLY | O_TRUNC, a2);
}

SYSCALL(close) {
    UNUSED_ARGS();
    return fd_close((int)a1);
}

/* Closes a range of descriptors, or marks them close-on-exec
 * (CLOSE_RANGE_CLOEXEC): a program about to start another leaves nothing of
 * its own open in it this way. */
SYSCALL(close_range) {
    UNUSED_ARGS();
    uint64_t first = (uint32_t)a1, last = (uint32_t)a2;
    if (first > last)
        return -EINVAL;
    struct fdtable *t = proc_current()->fds;
    for (uint64_t fd = first; fd <= last && fd < MAX_FDS; fd++) {
        if (!t->fd[fd])
            continue;
        if (a3 & 4 /* CLOSE_RANGE_CLOEXEC */)
            t->cloexec[fd] = 1;
        else
            fd_close((int)fd);
    }
    return 0;
}

/* ---- stat family ---- */

/* st_dev: the file system it is on (a bind mount keeps its source's), as on
 * Linux; things on no mount (pipes, sockets) share one of their own. Programs
 * tell file systems apart by it: Flatpak's document portal took every file
 * for one of its own while all said 1. */
#define NO_MOUNT_DEV 0x7fff

static void fill_stat(struct vnode *v, const struct mount *m, struct linux_stat *st) {
    memset(st, 0, sizeof *st);
    uint64_t minor = m ? (uint64_t)mount_dev(m) : NO_MOUNT_DEV; /* device 0:minor, as /proc/self/mountinfo says */
    st->st_dev = (minor & 0xff) | ((minor & 0xfff00) << 12);
    st->st_ino = v->ino;
    st->st_nlink = v->nlink;
    st->st_mode = vnode_stat_mode(v);
    st->st_uid = uid_to_user(v->uid); /* as the caller's user namespace names them */
    st->st_gid = gid_to_user(v->gid);
    st->st_rdev = v->rdev;
    st->st_size = (int64_t)v->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)(ALIGN_UP(v->size, 512) / 512);
    st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = v->mtime;
}

static int lookup_at_path(int dirfd, uint64_t upath, int flags, struct path *out) {
    char *path;
    int64_t r = user_path(upath, &path);
    if (r < 0)
        return (int)r;
    if (!path[0] && (flags & AT_EMPTY_PATH)) {
        kfree(path);
        if (dirfd == AT_FDCWD) {
            fs_context(NULL, out);
            return 0;
        }
        struct file *f = fd_get(dirfd);
        if (!f)
            return -EBADF;
        file_place(f, out);
        file_put(f);
        return 0;
    }
    struct path base;
    r = at_dir(dirfd, path, &base);
    if (r == 0) {
        r = vfs_lookup_path(&base, path, (flags & AT_SYMLINK_NOFOLLOW) ? 0 : LOOKUP_FOLLOW, out);
        path_put(&base);
    }
    kfree(path);
    return (int)r;
}

static int lookup_at(int dirfd, uint64_t upath, int flags, struct vnode **out) {
    struct path p;
    int r = lookup_at_path(dirfd, upath, flags, &p);
    if (r < 0)
        return r;
    *out = p.v;
    vnode_ref(p.v);
    path_put(&p);
    return 0;
}

static int64_t stat_at(int dirfd, uint64_t upath, uint64_t ust, int flags) {
    struct path p;
    int r = lookup_at_path(dirfd, upath, flags, &p);
    if (r < 0)
        return r;
    struct linux_stat st;
    fill_stat(p.v, p.mnt, &st);
    path_put(&p);
    return copy_to_user(ust, &st, sizeof st);
}

SYSCALL(stat) {
    UNUSED_ARGS();
    return stat_at(AT_FDCWD, a1, a2, 0);
}

SYSCALL(lstat) {
    UNUSED_ARGS();
    return stat_at(AT_FDCWD, a1, a2, AT_SYMLINK_NOFOLLOW);
}

SYSCALL(newfstatat) {
    UNUSED_ARGS();
    return stat_at((int)a1, a2, a3, (int)a4);
}

SYSCALL(fstat) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    struct linux_stat st;
    fill_stat(f->vnode, f->mnt, &st);
    file_put(f);
    return copy_to_user(a2, &st, sizeof st);
}

SYSCALL(statx) {
    UNUSED_ARGS();
    struct path p;
    int r = lookup_at_path((int)a1, a2, (int)a3, &p);
    if (r < 0)
        return r;
    struct linux_stat st;
    fill_stat(p.v, p.mnt, &st);
    struct linux_statx sx = {0};
    sx.stx_mask = 0x7ff | 0x1000; /* STATX_BASIC_STATS, STATX_MNT_ID */
    sx.stx_mnt_id = (uint64_t)mount_id(p.mnt);
    path_put(&p);
    sx.stx_blksize = 4096;
    sx.stx_nlink = (uint32_t)st.st_nlink;
    sx.stx_uid = st.st_uid;
    sx.stx_gid = st.st_gid;
    sx.stx_mode = (uint16_t)st.st_mode;
    sx.stx_ino = st.st_ino;
    sx.stx_size = (uint64_t)st.st_size;
    sx.stx_blocks = (uint64_t)st.st_blocks;
    sx.stx_atime.tv_sec = sx.stx_mtime.tv_sec = sx.stx_ctime.tv_sec = sx.stx_btime.tv_sec = st.st_mtim.tv_sec;
    sx.stx_rdev_major = (uint32_t)(st.st_rdev >> 8);
    sx.stx_rdev_minor = (uint32_t)(st.st_rdev & 0xff);
    sx.stx_dev_major = (uint32_t)((st.st_dev >> 8) & 0xfff); /* glibc's dev_t encoding */
    sx.stx_dev_minor = (uint32_t)((st.st_dev & 0xff) | ((st.st_dev >> 12) & 0xfff00));
    return copy_to_user(a5, &sx, sizeof sx);
}

static int64_t access_at(int dirfd, uint64_t upath, uint64_t mode, int flags) {
    struct vnode *v;
    int r = lookup_at(dirfd, upath, flags, &v);
    if (r < 0)
        return r;
    /* Single-user system for now: root may do anything except execute
     * files without any x bit, as on Linux. */
    if ((mode & 1) && v->type == VN_REG && !(v->mode & 0111))
        r = -EACCES;
    vnode_put(v);
    return r;
}

SYSCALL(access) {
    UNUSED_ARGS();
    return access_at(AT_FDCWD, a1, a2, 0);
}

SYSCALL(faccessat) {
    UNUSED_ARGS();
    return access_at((int)a1, a2, a3, 0);
}

SYSCALL(faccessat2) {
    UNUSED_ARGS();
    return access_at((int)a1, a2, a3, (int)a4);
}

/* ---- directories ---- */

SYSCALL(getdents64) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    struct vnode *v = f->vnode;
    if (v->type != VN_DIR || !v->ops->readdir) {
        file_put(f);
        return -ENOTDIR;
    }
    uint8_t *kb = kmalloc(a3 < BOUNCE ? a3 : BOUNCE);
    if (!kb) {
        file_put(f);
        return -ENOMEM;
    }
    size_t cap = a3 < BOUNCE ? a3 : BOUNCE, used = 0;
    char name[NAME_MAX + 1];
    for (;;) {
        uint8_t type;
        uint64_t ino;
        if (v->ops->readdir(v, f->offset, name, sizeof name, &type, &ino) <= 0)
            break;
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + strlen(name) + 1, 8);
        if (used + reclen > cap)
            break;
        struct linux_dirent64 *d = (void *)(kb + used);
        d->d_ino = ino;
        d->d_off = (int64_t)f->offset + 1;
        d->d_reclen = (uint16_t)reclen;
        d->d_type = type;
        memcpy(d->d_name, name, strlen(name) + 1);
        used += reclen;
        f->offset++;
    }
    int64_t r = used == 0 && cap < 280 ? -EINVAL : (int64_t)used;
    if (r > 0 && copy_to_user(a2, kb, used) < 0)
        r = -EFAULT;
    kfree(kb);
    file_put(f);
    return r;
}

SYSCALL(getcwd) {
    UNUSED_ARGS();
    char *buf = kmalloc(PATH_MAX);
    if (!buf)
        return -ENOMEM;
    struct path root, cwd;
    fs_context(&root, &cwd);
    int n = path_name(&cwd, &root, buf, PATH_MAX);
    path_put(&root);
    path_put(&cwd);
    int64_t r = n < 0 ? n : (uint64_t)n + 1 > a2 ? -ERANGE : copy_to_user(a1, buf, (size_t)n + 1) < 0 ? -EFAULT : n + 1;
    kfree(buf);
    return r;
}

/* Takes the caller's reference on `to`. */
static int set_cwd(struct path *to) {
    if (to->v->type != VN_DIR) {
        path_put(to);
        return -ENOTDIR;
    }
    struct process *p = proc_current();
    uint64_t lf = proc_table_lock(); /* /proc/<pid>/cwd takes references on it */
    struct path old = p->cwd;
    p->cwd = *to;
    proc_table_unlock(lf);
    path_put(&old);
    return 0;
}

SYSCALL(chdir) {
    UNUSED_ARGS();
    struct path to;
    int r = lookup_at_path(AT_FDCWD, a1, 0, &to);
    return r < 0 ? r : set_cwd(&to);
}

SYSCALL(fchdir) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    struct path to;
    file_place(f, &to);
    file_put(f);
    return set_cwd(&to);
}

static int64_t mkdir_at(int dirfd, uint64_t upath, uint64_t mode) {
    char *path;
    int64_t r = user_path(upath, &path);
    if (r < 0)
        return r;
    struct path dir;
    char name[NAME_MAX + 1];
    r = parent_at(dirfd, path, &dir, name, sizeof name);
    if (r == 0) {
        r = dir.v->ops->mkdir ? dir.v->ops->mkdir(dir.v, name, (uint32_t)mode & ~proc_current()->umask & 07777)
                              : -EROFS;
        path_put(&dir);
    }
    kfree(path);
    return r;
}

SYSCALL(mkdir) {
    UNUSED_ARGS();
    return mkdir_at(AT_FDCWD, a1, a2);
}

SYSCALL(mkdirat) {
    UNUSED_ARGS();
    return mkdir_at((int)a1, a2, a3);
}

static int64_t unlink_at(int dirfd, uint64_t upath, bool dir_only) {
    char *path;
    int64_t r = user_path(upath, &path);
    if (r < 0)
        return r;
    struct path dir;
    char name[NAME_MAX + 1];
    r = parent_at(dirfd, path, &dir, name, sizeof name);
    if (r == 0) {
        r = dir.v->ops->unlink ? dir.v->ops->unlink(dir.v, name, dir_only) : -EROFS;
        path_put(&dir);
    }
    kfree(path);
    return r;
}

SYSCALL(unlink) {
    UNUSED_ARGS();
    return unlink_at(AT_FDCWD, a1, false);
}

SYSCALL(rmdir) {
    UNUSED_ARGS();
    return unlink_at(AT_FDCWD, a1, true);
}

SYSCALL(unlinkat) {
    UNUSED_ARGS();
    return unlink_at((int)a1, a2, a3 & AT_REMOVEDIR);
}

static bool name_exists(struct vnode *dir, const char *name) {
    struct vnode *v;
    if (!dir->ops->lookup || dir->ops->lookup(dir, name, &v) < 0)
        return false;
    vnode_put(v);
    return true;
}

static int64_t rename_at(int odirfd, uint64_t uold, int ndirfd, uint64_t unew, uint64_t flags) {
    char *opath, *npath;
    int64_t r = user_path(uold, &opath);
    if (r < 0)
        return r;
    r = user_path(unew, &npath);
    if (r < 0) {
        kfree(opath);
        return r;
    }
    struct path odir = {0}, ndir = {0};
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    if ((r = parent_at(odirfd, opath, &odir, oname, sizeof oname)) == 0 &&
        (r = parent_at(ndirfd, npath, &ndir, nname, sizeof nname)) == 0) {
        if (odir.mnt != ndir.mnt && odir.mnt && ndir.mnt && odir.mnt != ndir.mnt)
            r = -EXDEV; /* across mounts, even of the same file system, as Linux */
        else
            r = (flags & 1) && name_exists(ndir.v, nname) ? -EEXIST /* RENAME_NOREPLACE */
                : odir.v->ops->rename ? odir.v->ops->rename(odir.v, oname, ndir.v, nname) : -EROFS;
    }
    path_put(&odir);
    path_put(&ndir);
    kfree(opath);
    kfree(npath);
    return r;
}

SYSCALL(rename) {
    UNUSED_ARGS();
    return rename_at(AT_FDCWD, a1, AT_FDCWD, a2, 0);
}

SYSCALL(renameat) {
    UNUSED_ARGS();
    return rename_at((int)a1, a2, (int)a3, a4, 0);
}

SYSCALL(renameat2) {
    UNUSED_ARGS();
    return a5 & ~1ull ? -EINVAL : rename_at((int)a1, a2, (int)a3, a4, a5); /* RENAME_EXCHANGE, WHITEOUT: not here */
}

static int64_t symlink_at(uint64_t utarget, int dirfd, uint64_t upath) {
    char *target;
    int64_t r = user_path(utarget, &target);
    if (r < 0)
        return r;
    char *path;
    r = user_path(upath, &path);
    if (r == 0 || r > 0) {
        struct path dir;
        char name[NAME_MAX + 1];
        r = parent_at(dirfd, path, &dir, name, sizeof name);
        if (r == 0) {
            r = dir.v->ops->symlink ? dir.v->ops->symlink(dir.v, name, target) : -EROFS;
            path_put(&dir);
        }
        kfree(path);
    }
    kfree(target);
    return r;
}

SYSCALL(symlink) {
    UNUSED_ARGS();
    return symlink_at(a1, AT_FDCWD, a2);
}

SYSCALL(symlinkat) {
    UNUSED_ARGS();
    return symlink_at(a1, (int)a2, a3);
}

static int64_t link_at(int odirfd, uint64_t uold, int ndirfd, uint64_t unew, int flags) {
    struct vnode *target;
    int r = lookup_at(odirfd, uold, (flags & AT_SYMLINK_FOLLOW) ? 0 : AT_SYMLINK_NOFOLLOW, &target);
    if (r < 0)
        return r;
    char *path;
    int64_t pr = user_path(unew, &path);
    if (pr < 0) {
        vnode_put(target);
        return pr;
    }
    struct path dir;
    char name[NAME_MAX + 1];
    r = parent_at(ndirfd, path, &dir, name, sizeof name);
    if (r == 0) {
        r = dir.v->ops->link ? dir.v->ops->link(dir.v, name, target) : -EPERM;
        path_put(&dir);
    }
    kfree(path);
    vnode_put(target);
    return r;
}

SYSCALL(link) {
    UNUSED_ARGS();
    return link_at(AT_FDCWD, a1, AT_FDCWD, a2, 0);
}

SYSCALL(linkat) {
    UNUSED_ARGS();
    return link_at((int)a1, a2, (int)a3, a4, (int)a5);
}

static int64_t readlink_at(int dirfd, uint64_t upath, uint64_t ubuf, uint64_t size) {
    struct vnode *v;
    int64_t r = lookup_at(dirfd, upath, AT_SYMLINK_NOFOLLOW, &v);
    if (r < 0)
        return r;
    if (v->type != VN_LNK || !v->ops->readlink) {
        vnode_put(v);
        return -EINVAL;
    }
    char *buf = kmalloc(PATH_MAX);
    if (!buf) {
        vnode_put(v);
        return -ENOMEM;
    }
    int n = v->ops->readlink(v, buf, size < PATH_MAX ? size : PATH_MAX);
    vnode_put(v);
    r = n < 0 ? n : copy_to_user(ubuf, buf, (size_t)n) < 0 ? -EFAULT : n;
    kfree(buf);
    return r;
}

SYSCALL(readlink) {
    UNUSED_ARGS();
    return readlink_at(AT_FDCWD, a1, a2, a3);
}

SYSCALL(readlinkat) {
    UNUSED_ARGS();
    return readlink_at((int)a1, a2, a3, a4);
}

static int64_t chmod_vnode(struct vnode *v, uint64_t mode) {
    v->mode = (uint32_t)mode & 07777;
    vnode_put(v);
    return 0;
}

static int64_t chmod_at(int dirfd, uint64_t upath, uint64_t mode) {
    struct path p;
    int r = lookup_at_path(dirfd, upath, 0, &p);
    if (r < 0)
        return r;
    if ((r = path_may_write(&p)) < 0) {
        path_put(&p);
        return r;
    }
    struct vnode *v = p.v;
    vnode_ref(v);
    path_put(&p); /* (clears p) */
    return chmod_vnode(v, mode);
}

SYSCALL(chmod) {
    UNUSED_ARGS();
    return chmod_at(AT_FDCWD, a1, a2);
}

SYSCALL(fchmodat) {
    UNUSED_ARGS();
    return chmod_at((int)a1, a2, a3);
}

SYSCALL(fchmod) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    f->vnode->mode = (uint32_t)a2 & 07777;
    file_put(f);
    return 0;
}

static int64_t do_truncate(struct vnode *v, uint64_t len) {
    if (v->type == VN_DIR)
        return -EISDIR;
    if (v->type != VN_REG || !v->ops->truncate)
        return -EINVAL;
    return v->ops->truncate(v, len);
}

SYSCALL(truncate) {
    UNUSED_ARGS();
    struct path p;
    int r = lookup_at_path(AT_FDCWD, a1, 0, &p);
    if (r < 0)
        return r;
    int64_t t = path_may_write(&p);
    if (t == 0)
        t = do_truncate(p.v, a2);
    path_put(&p);
    return t;
}

SYSCALL(ftruncate) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t t = do_truncate(f->vnode, a2);
    file_put(f);
    return t;
}

/* fallocate(fd, mode, offset, len): there are no blocks to reserve in a
 * memory file system, so only growing the size (mode 0, what
 * posix_fallocate asks for) does anything. */
SYSCALL(fallocate) {
    UNUSED_ARGS();
    int64_t off = (int64_t)a3, len = (int64_t)a4;
    if (off < 0 || len <= 0)
        return -EINVAL;
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    struct vnode *v = f->vnode;
    int64_t r = 0;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        r = -EBADF;
    else if (v->type == VN_FIFO || v->type == VN_SOCK)
        r = -ESPIPE;
    else if (v->type == VN_DIR)
        r = -EISDIR;
    else if (v->type != VN_REG || !v->ops->truncate)
        r = -ENODEV;
    else if (a2 & ~1ULL) /* only FALLOC_FL_KEEP_SIZE */
        r = -EOPNOTSUPP;
    else if (!(a2 & 1) && (uint64_t)(off + len) > v->size)
        r = v->ops->truncate(v, (uint64_t)(off + len));
    file_put(f);
    return r;
}

SYSCALL(umask) {
    UNUSED_ARGS();
    struct process *p = proc_current();
    uint32_t old = p->umask;
    p->umask = (uint32_t)a1 & 0777;
    return old;
}

/* ---- descriptor manipulation ---- */

static int64_t dup_to(int oldfd, int newfd, bool cloexec) {
    struct file *f = fd_get(oldfd);
    if (!f)
        return -EBADF;
    if (newfd < 0 || newfd >= MAX_FDS) {
        file_put(f);
        return -EBADF;
    }
    struct fdtable *t = proc_current()->fds;
    uint64_t fl = spin_lock_irqsave(&t->lock);
    struct file *old = t->fd[newfd];
    t->fd[newfd] = f;
    t->cloexec[newfd] = cloexec;
    spin_unlock_irqrestore(&t->lock, fl);
    if (old)
        file_put(old);
    return newfd;
}

SYSCALL(dup) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int r = fd_install(f, 0, false);
    if (r < 0)
        file_put(f);
    return r;
}

SYSCALL(dup2) {
    UNUSED_ARGS();
    if (a1 == a2) {
        struct file *f = fd_get((int)a1);
        if (!f)
            return -EBADF;
        file_put(f);
        return (int64_t)a2;
    }
    return dup_to((int)a1, (int)a2, false);
}

SYSCALL(dup3) {
    UNUSED_ARGS();
    if (a1 == a2)
        return -EINVAL;
    return dup_to((int)a1, (int)a2, a3 & O_CLOEXEC);
}

SYSCALL(fcntl) {
    UNUSED_ARGS();
    int fd = (int)a1;
    struct file *f = fd_get(fd);
    if (!f)
        return -EBADF;
    struct fdtable *t = proc_current()->fds;
    int64_t r = 0;
    switch (a2) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        file_ref(f);
        r = fd_install(f, (int)a3, a2 == F_DUPFD_CLOEXEC);
        if (r < 0)
            file_put(f);
        break;
    case F_GETFD:
        r = t->cloexec[fd] ? FD_CLOEXEC : 0;
        break;
    case F_SETFD:
        t->cloexec[fd] = (a3 & FD_CLOEXEC) != 0;
        break;
    case F_GETFL:
        r = f->flags;
        break;
    case F_SETFL:
        f->flags = (f->flags & ~(uint32_t)(O_APPEND | O_NONBLOCK)) | ((uint32_t)a3 & (O_APPEND | O_NONBLOCK));
        break;
    case F_ADD_SEALS:
        r = memfd_add_seals(f, (uint32_t)a3);
        break;
    case F_GET_SEALS:
        r = memfd_get_seals(f);
        break;
    /* Record locks: fs/locks.c */
    case F_SETLK:
    case F_SETLKW:
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
    case F_GETLK:
    case F_OFD_GETLK: {
        int64_t locks_fcntl(struct file *f, int cmd, uint64_t uarg);
        r = locks_fcntl(f, (int)a2, a3);
        break;
    }
    default:
        r = -EINVAL;
    }
    file_put(f);
    return r;
}

SYSCALL(ioctl) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t r;
    uint32_t req = (uint32_t)a2; /* Linux takes the request as 32 bits; musl sign-extends it */
    if (req == FIONBIO) {
        int on;
        r = copy_from_user(&on, a3, sizeof on);
        if (r == 0)
            f->flags = on ? f->flags | O_NONBLOCK : f->flags & ~(uint32_t)O_NONBLOCK;
    } else {
        r = f->vnode->ops->ioctl ? f->vnode->ops->ioctl(f->vnode, f, req, a3) : -ENOTTY;
    }
    file_put(f);
    return r;
}

static int64_t do_pipe(uint64_t ufds, uint64_t flags) {
    struct file *rd, *wr;
    int r = pipe_create(&rd, &wr);
    if (r < 0)
        return r;
    rd->flags |= (uint32_t)flags & O_NONBLOCK;
    wr->flags |= (uint32_t)flags & O_NONBLOCK;
    int fds[2];
    fds[0] = fd_install(rd, 0, flags & O_CLOEXEC);
    if (fds[0] < 0) {
        file_put(rd);
        file_put(wr);
        return fds[0];
    }
    fds[1] = fd_install(wr, 0, flags & O_CLOEXEC);
    if (fds[1] < 0) {
        fd_close(fds[0]);
        file_put(wr);
        return fds[1];
    }
    if (copy_to_user(ufds, fds, sizeof fds) < 0) {
        fd_close(fds[0]);
        fd_close(fds[1]);
        return -EFAULT;
    }
    return 0;
}

SYSCALL(pipe) {
    UNUSED_ARGS();
    return do_pipe(a1, 0);
}

SYSCALL(pipe2) {
    UNUSED_ARGS();
    return do_pipe(a1, a2);
}

/* ---- poll / select ---- */

int poll_file(struct file *f, struct waitq **wq) {
    if (wq)
        *wq = NULL;
    int ev = f->vnode->ops->poll ? f->vnode->ops->poll(f->vnode, f, wq) : (POLLIN | POLLOUT);
    if (ev & POLLIN)
        ev |= POLLRDNORM;
    if (ev & POLLOUT)
        ev |= POLLWRNORM;
    return ev;
}

int64_t user_timeout_ms(uint64_t uts) {
    if (!uts)
        return -1;
    struct linux_timespec ts;
    if (copy_from_user(&ts, uts, sizeof ts) < 0)
        return -EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
        return -EINVAL;
    return ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
}

static int poll_scan(struct linux_pollfd *fds, uint64_t n) {
    int ready = 0;
    for (uint64_t i = 0; i < n; i++) {
        struct linux_pollfd *p = &fds[i];
        p->revents = 0;
        if (p->fd < 0)
            continue;
        struct file *f = fd_get(p->fd);
        if (!f) {
            p->revents = POLLNVAL;
            ready++;
            continue;
        }
        int ev = poll_file(f, NULL);
        p->revents = (int16_t)(ev & (p->events | POLLERR | POLLHUP | POLLNVAL));
        if (p->revents)
            ready++;
        file_put(f);
    }
    return ready;
}

/* Waits until one of `fds` (kernel memory) is ready, the timeout (ms, -1 for
 * none) passes or a signal arrives. Rescans every tick rather than sleeping
 * on every file's wait queue at once: simple, and a tick is 4 ms. */
static int64_t poll_kernel(struct linux_pollfd *fds, uint64_t n, int64_t timeout_ms) {
    uint64_t deadline = timeout_ms < 0 ? UINT64_MAX : time_monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        int64_t r = poll_scan(fds, n);
        if (r > 0 || timeout_ms == 0 || time_monotonic_ns() >= deadline)
            return r;
        if (signal_pending_current())
            return -EINTR;
        thread_sleep_ms(4);
    }
}

static int64_t do_poll(uint64_t ufds, uint64_t n, int64_t timeout_ms) {
    if (n > MAX_FDS)
        return -EINVAL;
    struct linux_pollfd *fds = kmalloc(n ? n * sizeof *fds : 1);
    if (!fds)
        return -ENOMEM;
    if (copy_from_user(fds, ufds, n * sizeof *fds) < 0) {
        kfree(fds);
        return -EFAULT;
    }
    int64_t r = poll_kernel(fds, n, timeout_ms);
    if (r >= 0 && copy_to_user(ufds, fds, n * sizeof *fds) < 0)
        r = -EFAULT;
    kfree(fds);
    return r;
}

SYSCALL(poll) {
    UNUSED_ARGS();
    return do_poll(a1, a2, (int64_t)(int)a3);
}

SYSCALL(ppoll) {
    UNUSED_ARGS();
    int64_t timeout = user_timeout_ms(a3);
    if (timeout < -1)
        return timeout;
    if (a4) {
        int r = sigmask_push(a4, a5);
        if (r < 0)
            return r;
    }
    int64_t r = do_poll(a1, a2, timeout);
    if (a4)
        sigmask_pop(r);
    return r;
}

#define FD_SETSIZE 1024

/* select over poll: the three fd_set bitmaps become a pollfd list and back. */
static int64_t do_select(int64_t nfds, uint64_t ur, uint64_t uw, uint64_t ue, int64_t timeout_ms) {
    if (nfds < 0)
        return -EINVAL;
    if (nfds > FD_SETSIZE)
        nfds = FD_SETSIZE;
    uint64_t sets[3][FD_SETSIZE / 64] = {{0}};
    uint64_t usets[3] = {ur, uw, ue};
    size_t bytes = ALIGN_UP((uint64_t)nfds, 64) / 8;
    for (int k = 0; k < 3; k++)
        if (usets[k] && copy_from_user(sets[k], usets[k], bytes) < 0)
            return -EFAULT;
    static const int16_t want[3] = {POLLIN, POLLOUT, POLLPRI};
    uint64_t n = 0;
    for (int fd = 0; fd < nfds; fd++)
        if ((sets[0][fd / 64] | sets[1][fd / 64] | sets[2][fd / 64]) & (1ULL << (fd % 64))) {
            if (fd >= MAX_FDS)
                return -EBADF;
            n++;
        }
    struct linux_pollfd *fds = kmalloc(n ? n * sizeof *fds : 1);
    if (!fds)
        return -ENOMEM;
    uint64_t i = 0;
    for (int fd = 0; fd < nfds && i < n; fd++) {
        int16_t ev = 0;
        for (int k = 0; k < 3; k++)
            if (sets[k][fd / 64] & (1ULL << (fd % 64)))
                ev |= want[k];
        if (ev)
            fds[i++] = (struct linux_pollfd){.fd = fd, .events = ev};
    }
    int64_t r = poll_kernel(fds, n, timeout_ms);
    if (r >= 0) {
        memset(sets, 0, sizeof sets);
        r = 0;
        for (i = 0; i < n; i++) {
            int16_t rev = fds[i].revents;
            int fd = fds[i].fd;
            if (rev & POLLNVAL) {
                r = -EBADF;
                break;
            }
            /* Linux's POLLIN_SET, POLLOUT_SET and POLLEX_SET */
            bool hit[3] = {(rev & (POLLIN | POLLHUP | POLLERR)) != 0, (rev & (POLLOUT | POLLERR)) != 0,
                           (rev & POLLPRI) != 0};
            for (int k = 0; k < 3; k++)
                if (hit[k] && (fds[i].events & want[k])) {
                    sets[k][fd / 64] |= 1ULL << (fd % 64);
                    r++;
                }
        }
        for (int k = 0; r >= 0 && k < 3; k++)
            if (usets[k] && copy_to_user(usets[k], sets[k], bytes) < 0)
                r = -EFAULT;
    }
    kfree(fds);
    return r;
}

SYSCALL(select) {
    UNUSED_ARGS();
    int64_t timeout = -1;
    struct linux_timeval tv;
    if (a5) {
        if (copy_from_user(&tv, a5, sizeof tv) < 0)
            return -EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0)
            return -EINVAL;
        timeout = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
    }
    uint64_t start = time_monotonic_ns();
    int64_t r = do_select((int64_t)(int)a1, a2, a3, a4, timeout);
    if (a5 && r >= 0) { /* Linux reports the time left */
        uint64_t spent_us = (time_monotonic_ns() - start) / 1000, total_us = (uint64_t)timeout * 1000;
        uint64_t left = spent_us < total_us ? total_us - spent_us : 0;
        tv.tv_sec = (int64_t)(left / 1000000);
        tv.tv_usec = (int64_t)(left % 1000000);
        copy_to_user(a5, &tv, sizeof tv);
    }
    return r;
}

SYSCALL(pselect6) {
    UNUSED_ARGS();
    int64_t timeout = user_timeout_ms(a5);
    if (timeout < -1)
        return timeout;
    struct {
        uint64_t set, size;
    } sig = {0, 0};
    if (a6 && copy_from_user(&sig, a6, sizeof sig) < 0)
        return -EFAULT;
    if (sig.set) {
        int r = sigmask_push(sig.set, sig.size);
        if (r < 0)
            return r;
    }
    int64_t r = do_select((int64_t)(int)a1, a2, a3, a4, timeout);
    if (sig.set)
        sigmask_pop(r);
    return r;
}

SYSCALL(sendfile) {
    UNUSED_ARGS();
    struct file *out = fd_get((int)a1), *in = fd_get((int)a2);
    int64_t total = -EBADF;
    if (out && in) {
        uint64_t off = 0;
        bool use_off = a3 != 0;
        if (use_off && copy_from_user(&off, a3, sizeof off) < 0) {
            total = -EFAULT;
            goto done;
        }
        uint8_t *kb = kmalloc(BOUNCE);
        if (!kb) {
            total = -ENOMEM;
            goto done;
        }
        total = 0;
        while ((uint64_t)total < a4) {
            size_t want = a4 - (uint64_t)total < BOUNCE ? a4 - (uint64_t)total : BOUNCE;
            int64_t n = use_off ? file_pread(in, kb, want, off) : file_read(in, kb, want);
            if (n <= 0) {
                if (total == 0)
                    total = n;
                break;
            }
            int64_t w = file_write(out, kb, (size_t)n);
            if (w <= 0) {
                if (total == 0)
                    total = w;
                break;
            }
            total += w;
            off += (uint64_t)w;
            if (w < n)
                break;
        }
        kfree(kb);
        if (use_off && total >= 0)
            copy_to_user(a3, &off, sizeof off);
    }
done:
    if (out)
        file_put(out);
    if (in)
        file_put(in);
    return total;
}

/* ---- trivially-satisfied calls ---- */

SYSCALL(fsync) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    /* Files in memory have nothing to make durable; disks drain their cache. */
    struct vnode *v = f->vnode;
    int r = v->ops->fsync ? v->ops->fsync(v) : 0;
    file_put(f);
    return r;
}

/* Ownership. Giving a file away takes CAP_CHOWN; its owner may change its
 * group. Ids are as the caller's user namespace names them; -1 keeps one. */
bool proc_has_cap(int cap);

static int64_t chown_vnode(struct vnode *v, uint64_t uuid, uint64_t ugid) {
    struct process *p = proc_current();
    uint32_t ku = v->uid, kg = v->gid;
    if ((uint32_t)uuid != (uint32_t)-1 && !uid_from_user((uint32_t)uuid, &ku))
        return -EINVAL;
    if ((uint32_t)ugid != (uint32_t)-1 && !gid_from_user((uint32_t)ugid, &kg))
        return -EINVAL;
    if (!proc_has_cap(0) && (ku != v->uid || (kg != v->gid && p->euid != v->uid)))
        return -EPERM;
    v->uid = ku;
    v->gid = kg;
    return 0;
}

static int64_t chown_at(int dirfd, uint64_t upath, uint64_t uuid, uint64_t ugid, int flags) {
    struct vnode *v;
    int r = lookup_at(dirfd, upath, flags, &v);
    if (r < 0)
        return r;
    int64_t ret = chown_vnode(v, uuid, ugid);
    vnode_put(v);
    return ret;
}

SYSCALL(chown) {
    UNUSED_ARGS();
    return chown_at(AT_FDCWD, a1, a2, a3, 0);
}

SYSCALL(lchown) {
    UNUSED_ARGS();
    return chown_at(AT_FDCWD, a1, a2, a3, AT_SYMLINK_NOFOLLOW);
}

SYSCALL(fchownat) {
    UNUSED_ARGS();
    return chown_at((int)a1, a2, a3, a4, (int)a5);
}

SYSCALL(fchown) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    int64_t r = chown_vnode(f->vnode, a2, a3);
    file_put(f);
    return r;
}

SYSCALL(utimensat) {
    UNUSED_ARGS();
    return 0;
}

/* struct statfs as 64-bit words: the disk's file system says its real size
 * and free space (what's left for a downloaded model, say); everything else
 * lives in memory and answers with a generous fixed size. */
static void fill_statfs(struct vnode *v, uint64_t st[15]) {
    int ext2_statfs(struct vnode *v, uint64_t *st);
    memset(st, 0, 15 * sizeof st[0]);
    if (v && ext2_statfs(v, st) == 0)
        return;
    uint64_t mem[15] = {0x858458f6 /* RAMFS_MAGIC */, 4096, 1 << 20, 1 << 19, 1 << 19, 1 << 16, 1 << 15, 0, 255, 4096};
    memcpy(st, mem, sizeof mem);
}

SYSCALL(statfs) {
    UNUSED_ARGS();
    struct path p;
    int r = lookup_at_path(AT_FDCWD, a1, 0, &p);
    if (r < 0)
        return r;
    uint64_t st[15];
    fill_statfs(p.v, st);
    path_put(&p);
    return copy_to_user(a2, st, sizeof st);
}

SYSCALL(fstatfs) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    uint64_t st[15];
    fill_statfs(f->vnode, st);
    file_put(f);
    return copy_to_user(a2, st, sizeof st);
}

SYSCALL(sync) {
    UNUSED_ARGS();
    void ext2_sync_all(void);
    ext2_sync_all();
    blkdev_sync_all();
    return 0;
}

SYSCALL(flock) {
    UNUSED_ARGS();
    return 0;
}

/* syncfs: as sync (the disk file system keeps changes for a few seconds) */
SYSCALL(syncfs) {
    UNUSED_ARGS();
    void ext2_sync_all(void);
    ext2_sync_all();
    blkdev_sync_all();
    return 0;
}

/* mlock and friends: nothing here is ever swapped out, so memory is always "locked" */
SYSCALL(mlock) {
    UNUSED_ARGS();
    return 0;
}
