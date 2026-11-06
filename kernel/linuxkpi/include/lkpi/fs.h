#pragma once
/* kernel/linuxkpi: files and character devices, as NVIDIA's glue sees them: /dev/nvidiactl and /dev/nvidia0 are
 * character devices whose open, ioctl, mmap and poll the driver provides (file_operations); the layer joins those to
 * our kernel's vnodes (kernel/linuxkpi/fs.c). Also what the driver reads from files (firmware, its registry) and
 * the little of /proc it fills. */
#include <lkpi/core.h>
#include <lkpi/mm.h>
#include <lkpi/sched.h>

#define MINORBITS 20
#define MINORMASK ((1U << MINORBITS) - 1)
#define MAJOR(d) ((unsigned int)((d) >> MINORBITS))
#define MINOR(d) ((unsigned int)((d) & MINORMASK))
#define MKDEV(ma, mi) (((ma) << MINORBITS) | (mi))
typedef long off_t;

#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR 02
#define O_ACCMODE 03
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#define O_CLOEXEC 02000000
#define O_LARGEFILE 0100000
#define FMODE_READ 0x1
#define FMODE_WRITE 0x2
#define S_IFMT 00170000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLRDNORM 0x0040
#define POLLWRNORM 0x0100
typedef unsigned int __poll_t;
#define EPOLLIN POLLIN
#define EPOLLPRI POLLPRI
#define EPOLLOUT POLLOUT
#define EPOLLERR POLLERR
#define EPOLLRDNORM POLLRDNORM

struct super_block;
struct address_space_operations;
struct address_space { struct inode *host; void *private_data; struct rw_semaphore i_mmap_rwsem; const struct address_space_operations *a_ops; };
struct inode {
    umode_t i_mode;
    dev_t i_rdev;
    loff_t i_size;
    unsigned long i_ino;
    void *i_private;
    struct address_space *i_mapping;
    struct address_space i_data;
    struct cdev *i_cdev;
    struct super_block *i_sb;
};
static inline unsigned int iminor(const struct inode *i) { return MINOR(i->i_rdev); }
static inline unsigned int imajor(const struct inode *i) { return MAJOR(i->i_rdev); }
struct path { void *mnt; void *dentry; };
struct file_operations;
struct file {
    const struct file_operations *f_op;
    struct inode *f_inode;
    struct address_space *f_mapping;
    unsigned int f_flags;
    fmode_t f_mode;
    loff_t f_pos;
    void *private_data;
    struct path f_path;
    const struct cred *f_cred;
    void *lkpi_file; /* our kernel's open file */
};
static inline struct inode *file_inode(const struct file *f) { return f->f_inode; }
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
void poll_wait(struct file *f, wait_queue_head_t *q, poll_table *p);
struct kiocb;
struct iov_iter;
struct file_operations {
    void *owner;
    loff_t (*llseek)(struct file *f, loff_t off, int whence);
    ssize_t (*read)(struct file *f, char __user *buf, size_t n, loff_t *pos);
    ssize_t (*write)(struct file *f, const char __user *buf, size_t n, loff_t *pos);
    __poll_t (*poll)(struct file *f, poll_table *p);
    long (*unlocked_ioctl)(struct file *f, unsigned int cmd, unsigned long arg);
    long (*compat_ioctl)(struct file *f, unsigned int cmd, unsigned long arg);
    int (*mmap)(struct file *f, struct vm_area_struct *v);
    int (*open)(struct inode *i, struct file *f);
    int (*flush)(struct file *f, void *id);
    int (*release)(struct inode *i, struct file *f);
    int (*fsync)(struct file *f, loff_t a, loff_t b, int datasync);
    unsigned long (*get_unmapped_area)(struct file *f, unsigned long a, unsigned long l, unsigned long p, unsigned long fl);
};
loff_t noop_llseek(struct file *f, loff_t off, int whence);
loff_t no_llseek(struct file *f, loff_t off, int whence);
int nonseekable_open(struct inode *i, struct file *f);
struct file *fget(unsigned int fd);
void fput(struct file *f);
int get_unused_fd_flags(unsigned int flags);
void put_unused_fd(unsigned int fd);
void fd_install(unsigned int fd, struct file *f);
int close_fd(unsigned int fd);
struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags);
int iterate_fd(void *files, unsigned int n, int (*f)(const void *p, struct file *file, unsigned int fd), const void *p);
struct file *filp_open(const char *path, int flags, umode_t mode);
int filp_close(struct file *f, void *id);
ssize_t kernel_read(struct file *f, void *buf, size_t n, loff_t *pos);
ssize_t kernel_write(struct file *f, const void *buf, size_t n, loff_t *pos);
struct file *shmem_kernel_file_setup(const char *name, loff_t size, unsigned long flags);
void address_space_init_once(struct address_space *m);

/* character devices */
struct cdev { struct kobject kobj; void *owner; const struct file_operations *ops; dev_t dev; unsigned int count; struct list_head list; };
void cdev_init(struct cdev *c, const struct file_operations *fops);
struct cdev *cdev_alloc(void);
int cdev_add(struct cdev *c, dev_t dev, unsigned int count);
void cdev_del(struct cdev *c);
int alloc_chrdev_region(dev_t *dev, unsigned int first, unsigned int count, const char *name);
int register_chrdev_region(dev_t from, unsigned int count, const char *name);
void unregister_chrdev_region(dev_t from, unsigned int count);
int __register_chrdev(unsigned int major, unsigned int first, unsigned int count, const char *name, const struct file_operations *f);
void __unregister_chrdev(unsigned int major, unsigned int first, unsigned int count, const char *name);
#define register_chrdev(ma, n, f) __register_chrdev(ma, 0, 256, n, f)
#define unregister_chrdev(ma, n) __unregister_chrdev(ma, 0, 256, n)

/* ioctl numbers */
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT 8
#define _IOC_SIZESHIFT 16
#define _IOC_DIRSHIFT 30
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size) (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(t, nr) _IOC(_IOC_NONE, (t), (nr), 0)
#define _IOR(t, nr, sz) _IOC(_IOC_READ, (t), (nr), sizeof(sz))
#define _IOW(t, nr, sz) _IOC(_IOC_WRITE, (t), (nr), sizeof(sz))
#define _IOWR(t, nr, sz) _IOC(_IOC_READ | _IOC_WRITE, (t), (nr), sizeof(sz))
#define _IOC_NR(n) (((n) >> _IOC_NRSHIFT) & ((1 << _IOC_NRBITS) - 1))
#define _IOC_TYPE(n) (((n) >> _IOC_TYPESHIFT) & ((1 << _IOC_TYPEBITS) - 1))
#define _IOC_SIZE(n) (((n) >> _IOC_SIZESHIFT) & ((1 << _IOC_SIZEBITS) - 1))
#define _IOC_DIR(n) (((n) >> _IOC_DIRSHIFT) & 3)

/* /proc and seq_file (the driver's /proc/driver/nvidia: kept small, read by nvidia-smi only for details) */
struct seq_file { char *buf; size_t size, count; void *private; const void *op; struct file *file; };
struct proc_dir_entry;
struct proc_ops {
    int (*proc_open)(struct inode *i, struct file *f);
    ssize_t (*proc_read)(struct file *f, char __user *b, size_t n, loff_t *p);
    ssize_t (*proc_write)(struct file *f, const char __user *b, size_t n, loff_t *p);
    loff_t (*proc_lseek)(struct file *f, loff_t o, int w);
    int (*proc_release)(struct inode *i, struct file *f);
    unsigned int proc_flags;
};
#define PROC_ENTRY_PERMANENT 1
struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent);
struct proc_dir_entry *proc_mkdir_mode(const char *name, umode_t mode, struct proc_dir_entry *parent);
struct proc_dir_entry *proc_create_data(const char *name, umode_t mode, struct proc_dir_entry *parent, const struct proc_ops *ops, void *data);
#define proc_create(n, m, p, o) proc_create_data(n, m, p, o, NULL)
struct proc_dir_entry *proc_symlink(const char *name, struct proc_dir_entry *parent, const char *dest);
void proc_remove(struct proc_dir_entry *e);
void remove_proc_entry(const char *name, struct proc_dir_entry *parent);
void proc_remove_subtree_lkpi(struct proc_dir_entry *e);
#define remove_proc_subtree(n, p) (remove_proc_entry(n, p), 0)
void *pde_data(const struct inode *i);
#define PDE_DATA pde_data
int single_open(struct file *f, int (*show)(struct seq_file *s, void *v), void *data);
int single_open_size(struct file *f, int (*show)(struct seq_file *s, void *v), void *data, size_t size);
int single_release(struct inode *i, struct file *f);
ssize_t seq_read(struct file *f, char __user *b, size_t n, loff_t *p);
loff_t seq_lseek(struct file *f, loff_t o, int w);
void seq_printf(struct seq_file *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void seq_puts(struct seq_file *s, const char *str);
void seq_putc(struct seq_file *s, char c);
void seq_write(struct seq_file *s, const void *d, size_t n);
struct va_format { const char *fmt; va_list *va; };

/* firmware (the GSP's, downloaded once to /lib/firmware/nvidia/<version>/ on this computer: never shipped) */
struct firmware { size_t size; const u8 *data; void *priv; };
int request_firmware(const struct firmware **fw, const char *name, struct device *d);
int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *d);
void release_firmware(const struct firmware *fw);
#define MODULE_FIRMWARE(f) extern const char __lkpi_firmware_unused __attribute__((unused))

/* modules: the driver is built into the kernel; these say who it is and take its parameters */
#define THIS_MODULE ((void *)0)
#define MODULE_LICENSE(l)
#define MODULE_AUTHOR(a)
#define MODULE_DESCRIPTION(d)
#define MODULE_VERSION(v)
#define MODULE_ALIAS(a)
#define MODULE_INFO(t, i)
#define MODULE_IMPORT_NS(n)
#define module_param(name, type, perm) extern int __lkpi_param_##name __attribute__((unused))
#define module_param_named(name, var, type, perm) extern int __lkpi_param_##name __attribute__((unused))
#define module_param_string(name, s, len, perm) extern int __lkpi_param_##name __attribute__((unused))
#define module_param_array(name, type, nump, perm) extern int __lkpi_param_##name __attribute__((unused))
#define MODULE_PARM_DESC(p, d)
#define __init
#define __exit
#define __initdata
#define __exitdata
#define __devinit
#define __devexit
#define module_init(fn) int lkpi_module_init_##fn(void) { return fn(); }
#define module_exit(fn) void lkpi_module_exit_##fn(void) { fn(); }
bool try_module_get(void *m);
void module_put(void *m);
#define __module_get(m) do { } while (0)
struct kernel_param;
struct kernel_param_ops { int (*set)(const char *v, const struct kernel_param *kp); int (*get)(char *b, const struct kernel_param *kp); };

/* shrinkers: the driver gives back memory when asked (the layer asks when ours runs short) */
struct shrink_control { gfp_t gfp_mask; int nid; unsigned long nr_to_scan; unsigned long nr_scanned; };
struct shrinker {
    unsigned long (*count_objects)(struct shrinker *s, struct shrink_control *sc);
    unsigned long (*scan_objects)(struct shrinker *s, struct shrink_control *sc);
    long batch;
    int seeks;
    unsigned int flags;
    void *private_data;
    struct list_head list;
};
#define SHRINK_STOP (~0UL)
#define SHRINK_EMPTY (~0UL - 1)
#define DEFAULT_SEEKS 2
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
void shrinker_register(struct shrinker *s);
void shrinker_free(struct shrinker *s);
int register_shrinker(struct shrinker *s, const char *fmt, ...);
void unregister_shrinker(struct shrinker *s);
