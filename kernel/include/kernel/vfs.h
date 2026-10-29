#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/spinlock.h>

/* Virtual file system. Every file system (ramfs now; osfs, FAT32 and
 * ext4 later) exposes vnodes with these operations. Errors are returned as
 * negative Linux errno values so system calls can pass them straight up. */

/* VN_ANON: eventfd, epoll, timerfd and signalfd objects, which live in no
 * directory and stat as neither files nor devices (like Linux anon inodes). */
/* VN_BLK: disks (/dev/sda); seekable like regular files, with a fixed size. */
enum vnode_type { VN_REG, VN_DIR, VN_CHR, VN_LNK, VN_FIFO, VN_SOCK, VN_ANON, VN_BLK };

struct vnode;
struct file;
struct waitq;
struct pageobj;
struct mount;
struct mntns;

/* A place in the tree: a vnode as reached through a mount. Once bind mounts
 * and mount namespaces exist, the same directory can be visible at several
 * places, so "where am I" (cwd, root, an open directory) is this pair. */
struct path {
    struct mount *mnt;
    struct vnode *v;
};

struct vnode_ops {
    /* directories */
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, uint32_t mode, struct vnode **out);
    int (*mkdir)(struct vnode *dir, const char *name, uint32_t mode);
    int (*symlink)(struct vnode *dir, const char *name, const char *target);
    int (*link)(struct vnode *dir, const char *name, struct vnode *target);
    int (*unlink)(struct vnode *dir, const char *name, bool dir_only);
    int (*rename)(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname);
    /* Returns 1 and fills name/type/ino for entry `index`, 0 at the end. */
    int (*readdir)(struct vnode *dir, uint64_t index, char *name, size_t name_size, uint8_t *type, uint64_t *ino);
    /* files and devices */
    int64_t (*read)(struct vnode *v, struct file *f, void *buf, size_t len, uint64_t off);
    int64_t (*write)(struct vnode *v, struct file *f, const void *buf, size_t len, uint64_t off);
    int (*truncate)(struct vnode *v, uint64_t size);
    int (*readlink)(struct vnode *v, char *buf, size_t size);
    /* "Magic" links (/proc/<pid>/fd/N, cwd, exe): following one yields the
     * object itself (referenced), even one with no path such as a pipe. Its
     * mount is filled in (referenced) when known, else left NULL. */
    int (*follow)(struct vnode *v, struct path *out);
    int64_t (*ioctl)(struct vnode *v, struct file *f, uint64_t req, uint64_t arg);
    /* Returns POLL* bits ready now; registers on `wq` for wakeups if non-NULL. */
    int (*poll)(struct vnode *v, struct file *f, struct waitq **wq);
    int (*open)(struct vnode *v, struct file *f);
    /* Devices backed by fixed physical memory (framebuffers): returns the
     * physical address of `off` so mmap can map it directly. */
    int (*mmap_phys)(struct vnode *v, uint64_t off, uint64_t len, uint64_t *phys);
    /* Files kept in pages (ramfs, memfd): returns the backing object, with
     * a reference, for MAP_SHARED. May refuse a writable mapping (seals). */
    int (*mmap_shared)(struct vnode *v, uint32_t prot, struct pageobj **out);
    void (*close)(struct vnode *v, struct file *f);
    void (*release)(struct vnode *v); /* last reference dropped */
    /* fsync/fdatasync: make written data durable (block devices). */
    int (*fsync)(struct vnode *v);
    /* Synthetic trees make a new vnode per lookup: is `b` the same node as `v`? */
    bool (*same)(struct vnode *v, struct vnode *b);
};

struct vnode {
    enum vnode_type type;
    uint32_t mode; /* permission bits only */
    uint32_t uid, gid;
    uint64_t ino;
    uint64_t size;
    uint32_t nlink;
    uint32_t rdev;
    int64_t mtime;
    int refcount;
    struct vnode *parent; /* directories: for ".." */
    struct vnode *mounted; /* root of a file system mounted here */
    const struct vnode_ops *ops;
    void *data;
    spinlock_t lock;
};

struct file {
    struct vnode *vnode;
    uint64_t offset;
    uint32_t flags; /* O_* status flags */
    int refcount;
    void *private;
    int epoll_refs; /* epoll registrations of this file (see epoll_file_closed) */
    spinlock_t lock;
    /* How it was opened, for /proc/<pid>/fd and maps: the path as given
     * (kmalloc'd) and, if relative, the directory it started from (referenced). */
    char *path;
    struct path path_base;
    struct mount *mnt; /* the mount it was reached through (referenced; NULL: none) */
};

void vfs_init(void);
struct vnode *vfs_root(void);
struct vnode *vnode_alloc(enum vnode_type type, uint32_t mode, const struct vnode_ops *ops);
void vnode_ref(struct vnode *v);
void vnode_put(struct vnode *v);
uint32_t vnode_stat_mode(const struct vnode *v);
/* Files with a position: reads/writes advance it, pread/pwrite work. */
static inline bool vnode_seekable(const struct vnode *v) { return v->type == VN_REG || v->type == VN_BLK; }

#define LOOKUP_FOLLOW  1 /* follow a symlink in the last component */
#define LOOKUP_PARENT  2 /* stop at the parent; return last component name */

/* Resolves `path` from `base` (NULL: the calling process's working
 * directory); absolute paths start at the process's root. */
int vfs_lookup(const struct path *base, const char *path, int flags, struct vnode **out);
int vfs_lookup_path(const struct path *base, const char *path, int flags, struct path *out);
int vfs_lookup_parent(const struct path *base, const char *path, struct vnode **dir, char *name, size_t name_size);
int vfs_lookup_parent_path(const struct path *base, const char *path, struct path *dir, char *name,
                           size_t name_size);
int vfs_open(const struct path *base, const char *path, uint32_t flags, uint32_t mode, struct file **out);
int vfs_mkdir_p(const char *path, uint32_t mode);
int vfs_create_node(const char *path, struct vnode *node);

struct file *file_from_vnode(struct vnode *v, uint32_t flags);
void file_ref(struct file *f);
void file_put(struct file *f);
int64_t file_read(struct file *f, void *buf, size_t len);
int64_t file_write(struct file *f, const void *buf, size_t len);
int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off);
/* Reads a whole file into a fresh kmalloc buffer. */
int vfs_read_all(struct vnode *v, void **buf, size_t *len);

/* ramfs: in-memory file system used for the root and initramfs. */
struct vnode *ramfs_create_root(void);
int ramfs_add_node(struct vnode *dir, const char *name, struct vnode *node);
struct pageobj;
int ramfs_add_object_file(const char *path, struct pageobj *o, uint32_t mode);

/* Mounts the file system whose root is `fsroot` on the directory at `path`
 * (boot-time mounts in the first namespace: /proc, /sys). */
int vfs_mount(const char *path, struct vnode *fsroot, const char *fstype, uint32_t flags);
/* Canonical absolute path of `path` (relative to `base`, NULL: the cwd),
 * symlinks resolved. Returns the length (without the NUL) or a negative errno. */
int vfs_realpath(const struct path *base, const char *path, char *out, size_t size);

/* ---- mounts and mount namespaces (fs/mount.c) ---- */

#define MNT_RDONLY 1
#define MNT_NOSUID 2
#define MNT_NODEV  4
#define MNT_NOEXEC 8

void mount_init(struct vnode *rootfs);
void path_get(struct path *p); /* takes a reference on both halves */
void path_put(struct path *p);
static inline bool path_equal(const struct path *a, const struct path *b) { return a->mnt == b->mnt && a->v == b->v; }
/* If something is mounted at `p`, moves it onto the topmost such mount. */
void path_cross(struct path *p);
/* "..": up one directory, crossing out of mounts, never above `root`. */
void path_parent(struct path *p, const struct path *root);
/* The name of `p` as seen from `root` ("/a/b"). Returns the length without
 * the NUL, or a negative errno. */
int path_name(const struct path *p, const struct path *root, char *buf, size_t size);
/* Absolute name of `p` as the calling process sees it. */
int path_name_here(const struct path *p, char *buf, size_t size);
/* Where a vnode reached without a path (a magic link) is: a mount whose
 * root it is, else the root of the caller's namespace. Referenced. */
void path_for_vnode(struct vnode *v, struct path *out);
/* -EROFS if `p` is on a read-only mount. */
int path_may_write(const struct path *p);
struct mount *mount_get(struct mount *m);
void mount_put(struct mount *m);
uint32_t mount_flags(const struct mount *m);
/* The mount's id (as in mountinfo) and device number. */
int mount_id(const struct mount *m);
int mount_dev(const struct mount *m);

/* The calling process's root and working directory, referenced. */
void fs_context(struct path *root, struct path *cwd);
/* Mount namespaces (held by processes). */
struct mntns *mntns_get(struct mntns *ns);
void mntns_put(struct mntns *ns);
/* A copy of `ns` for unshare/clone(CLONE_NEWNS); `root` and `cwd` (in the
 * old namespace) are moved over to the copies of their mounts. */
struct mntns *mntns_copy(struct mntns *ns, struct path *root, struct path *cwd);
uint64_t mntns_id(const struct mntns *ns);
/* /proc/<pid>/mounts and mountinfo, as seen from `root` in `ns`. */
struct sbuf;
void mounts_show(struct mntns *ns, const struct path *root, struct sbuf *b, bool info);
/* What /proc/<pid>/fd/N reads as: a path, "pipe:[ino]", "socket:[ino]"... */
int file_describe(struct file *f, char *out, size_t size);

/* /proc and /sys (fs/procfs.c, fs/sysfs.c) */
void procfs_init(void);
void sysfs_init(void);

/* initramfs (cpio newc) unpacked into the root. */
void initramfs_unpack(const void *data, size_t len);

/* pipes */
int pipe_create(struct file **read_end, struct file **write_end);

/* memfd: anonymous in-memory files with seals (fcntl F_ADD_SEALS). */
struct vnode *memfd_vnode_create(void);
int memfd_add_seals(struct file *f, uint32_t seals);
int memfd_get_seals(struct file *f);

/* epoll: drops every registration of `f`, which is being closed for good. */
void epoll_file_closed(struct file *f);

/* devices */
void devfs_init(void);
void tty_init(void);
void pty_init(void);
struct vnode *pty_slave_vnode(int index); /* referenced, or NULL */
void tty_input(char c); /* from keyboard / serial interrupt handlers */
