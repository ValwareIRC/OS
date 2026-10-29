#include <kernel/lib.h>
#include <kernel/mm.h>
#include <kernel/ns.h>
#include <kernel/proc.h>
#include <kernel/vfs.h>
#include <linux/abi.h>
#include <linux/syscall.h>

#include "../fs/synth.h"

/* Namespaces other than mounts (see kernel/ns.h). */

#define CLONE_NEWTIME   0x00000080ULL
#define CLONE_NEWNS     0x00020000ULL
#define CLONE_NEWCGROUP 0x02000000ULL
#define CLONE_NEWUTS    0x04000000ULL
#define CLONE_NEWIPC    0x08000000ULL
#define CLONE_NEWUSER   0x10000000ULL
#define CLONE_NEWPID    0x20000000ULL
#define CLONE_NEWNET    0x40000000ULL
#define CLONE_NS_ALL (CLONE_NEWTIME | CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | \
                      CLONE_NEWPID | CLONE_NEWNET)

struct pidns {
    int refcount, level;
    struct pidns *parent; /* referenced */
    int next_nr;
    int reaper; /* global id of its pid 1, or 0 */
    uint64_t id;
};

struct idmap {
    int n;
    struct {
        uint32_t first, lower, count;
    } e[5];
};

struct userns {
    int refcount, level;
    struct userns *parent; /* referenced */
    struct idmap uid, gid;
    bool setgroups_deny;
    uint32_t owner; /* global uid of its creator */
    uint64_t id;
    int max_userns; /* user.max_user_namespaces as set in this one */
    int nested;     /* user namespaces alive below it */
};

struct netns {
    int refcount;
    uint64_t id;
    bool initial;
};

struct utsns {
    int refcount;
    uint64_t id;
    char host[65], domain[65];
};

struct nsobj {
    int refcount;
    uint64_t id;
};

static spinlock_t ns_lock;
static uint64_t next_id = 4026531841ULL; /* the first mount namespace has ...840 */
static struct pidns first_pid = {.refcount = 1, .next_nr = 1};
#define DEFAULT_MAX_USERNS 63785
static struct userns first_user = {.refcount = 1, .max_userns = DEFAULT_MAX_USERNS};
static struct netns first_net = {.refcount = 1, .initial = true};
static struct utsns first_uts = {.refcount = 1, .host = "os", .domain = "(none)"};
static struct nsobj first_ipc = {.refcount = 1}, first_cgroup = {.refcount = 1}, first_time = {.refcount = 1};

uint64_t ns_next_id(void);
uint64_t ns_next_id(void) { return __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED); }

static void *ref(void *obj) {
    if (obj)
        __atomic_add_fetch((int *)obj, 1, __ATOMIC_RELAXED); /* refcount is each struct's first field */
    return obj;
}

static bool unref(void *obj) { return obj && __atomic_sub_fetch((int *)obj, 1, __ATOMIC_ACQ_REL) == 0; }

static void pidns_put(struct pidns *ns) {
    while (ns && ns->level > 0 && unref(ns)) {
        struct pidns *parent = ns->parent;
        kfree(ns);
        ns = parent;
    }
}

static void userns_put(struct userns *ns) {
    while (ns && ns->level > 0 && unref(ns)) {
        struct userns *parent = ns->parent;
        uint64_t f = spin_lock_irqsave(&ns_lock);
        for (struct userns *a = parent; a; a = a->parent)
            a->nested--;
        spin_unlock_irqrestore(&ns_lock, f);
        kfree(ns);
        ns = parent;
    }
}

struct netns *netns_get(struct netns *ns) { return ref(ns); }

void netns_put(struct netns *ns) {
    if (ns && !ns->initial && unref(ns))
        kfree(ns);
}

static void plain_put(void *obj, void *first) {
    if (obj && obj != first && unref(obj))
        kfree(obj);
}

void ns_init(struct ns_set *s) {
    first_pid.id = ns_next_id();
    first_user.id = ns_next_id();
    first_net.id = ns_next_id();
    first_uts.id = ns_next_id();
    first_ipc.id = ns_next_id();
    first_cgroup.id = ns_next_id();
    first_time.id = ns_next_id();
    s->pid = ref(&first_pid);
    s->pid_children = ref(&first_pid);
    s->user = ref(&first_user);
    s->net = ref(&first_net);
    s->uts = ref(&first_uts);
    s->ipc = ref(&first_ipc);
    s->cgroup = ref(&first_cgroup);
    s->time = ref(&first_time);
}

void ns_set_copy(struct ns_set *dst, const struct ns_set *src) {
    *dst = *src;
    ref(dst->pid);
    ref(dst->pid_children);
    ref(dst->user);
    ref(dst->net);
    ref(dst->uts);
    ref(dst->ipc);
    ref(dst->cgroup);
    ref(dst->time);
}

void ns_set_put(struct ns_set *s) {
    pidns_put(s->pid);
    pidns_put(s->pid_children);
    userns_put(s->user);
    netns_put(s->net);
    plain_put(s->uts, &first_uts);
    plain_put(s->ipc, &first_ipc);
    plain_put(s->cgroup, &first_cgroup);
    plain_put(s->time, &first_time);
    *s = (struct ns_set){0};
}

static struct pidns *pidns_new(struct pidns *parent) {
    if (parent->level + 1 >= PIDNS_MAX)
        return NULL;
    struct pidns *ns = kzalloc(sizeof *ns);
    if (!ns)
        return NULL;
    ns->refcount = 1;
    ns->level = parent->level + 1;
    ns->parent = ref(parent);
    ns->next_nr = 1;
    ns->id = ns_next_id();
    return ns;
}

/* A new user namespace counts against every one above it, each within its
 * own user.max_user_namespaces (how bubblewrap stops a sandbox from making
 * more). */
static int userns_new(struct userns *parent, uint32_t owner, struct userns **out) {
    if (parent->level + 1 >= 32)
        return -ENOSPC; /* too deep */
    struct userns *ns = kzalloc(sizeof *ns);
    if (!ns)
        return -ENOMEM;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    for (struct userns *a = parent; a; a = a->parent)
        if (a->nested + 1 > a->max_userns) {
            spin_unlock_irqrestore(&ns_lock, f);
            kfree(ns);
            return -ENOSPC;
        }
    for (struct userns *a = parent; a; a = a->parent)
        a->nested++;
    spin_unlock_irqrestore(&ns_lock, f);
    ns->refcount = 1;
    ns->level = parent->level + 1;
    ns->parent = ref(parent);
    ns->owner = owner;
    ns->id = ns_next_id();
    ns->max_userns = DEFAULT_MAX_USERNS;
    *out = ns;
    return 0;
}

int userns_max_get(void) {
    struct userns *u = proc_current()->ns.user;
    return (u ? u : &first_user)->max_userns;
}

int userns_max_set(int v) {
    if (v < 0)
        return -EINVAL;
    if (!ns_capable())
        return -EPERM;
    struct userns *u = proc_current()->ns.user;
    (u ? u : &first_user)->max_userns = v;
    return 0;
}

static struct netns *netns_new(void) {
    struct netns *ns = kzalloc(sizeof *ns);
    if (ns) {
        ns->refcount = 1;
        ns->id = ns_next_id();
    }
    return ns;
}

static struct utsns *utsns_copy(const struct utsns *from) {
    struct utsns *ns = kzalloc(sizeof *ns);
    if (ns) {
        *ns = *from;
        ns->refcount = 1;
        ns->id = ns_next_id();
    }
    return ns;
}

static struct nsobj *nsobj_new(void) {
    struct nsobj *ns = kzalloc(sizeof *ns);
    if (ns) {
        ns->refcount = 1;
        ns->id = ns_next_id();
    }
    return ns;
}

/* ---- process ids ---- */

struct pidlink {
    int gid;
    struct pidns *ns; /* referenced */
    int nr[PIDNS_MAX];
    struct pidlink *next;
};

#define PIDHASH 256
static struct pidlink *pidhash[PIDHASH];

static struct pidns *ancestor(struct pidns *ns, int level) {
    while (ns && ns->level > level)
        ns = ns->parent;
    return ns && ns->level == level ? ns : NULL;
}

int pid_register(struct pidns *ns, int gid) {
    if (!ns || ns->level == 0)
        return gid;
    struct pidlink *l = kzalloc(sizeof *l);
    if (!l)
        return -ENOMEM;
    l->gid = gid;
    l->ns = ref(ns);
    l->nr[0] = gid;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    for (struct pidns *a = ns; a && a->level > 0; a = a->parent)
        l->nr[a->level] = a->next_nr++;
    if (l->nr[ns->level] == 1)
        ns->reaper = gid;
    struct pidlink **b = &pidhash[(unsigned)gid % PIDHASH];
    l->next = *b;
    *b = l;
    spin_unlock_irqrestore(&ns_lock, f);
    return l->nr[ns->level];
}

void pid_unregister(int gid) {
    uint64_t f = spin_lock_irqsave(&ns_lock);
    struct pidlink *found = NULL;
    for (struct pidlink **pp = &pidhash[(unsigned)gid % PIDHASH]; *pp; pp = &(*pp)->next)
        if ((*pp)->gid == gid) {
            found = *pp;
            *pp = found->next;
            break;
        }
    if (found && found->ns->reaper == gid)
        found->ns->reaper = 0;
    spin_unlock_irqrestore(&ns_lock, f);
    if (found) {
        pidns_put(found->ns);
        kfree(found);
    }
}

/* ns_lock held */
static struct pidlink *link_of(int gid) {
    for (struct pidlink *l = pidhash[(unsigned)gid % PIDHASH]; l; l = l->next)
        if (l->gid == gid)
            return l;
    return NULL;
}

int pid_in(struct pidns *viewer, int gid) {
    if (gid <= 0 || !viewer || viewer->level == 0)
        return gid;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    struct pidlink *l = link_of(gid);
    int nr = l && ancestor(l->ns, viewer->level) == viewer ? l->nr[viewer->level] : 0;
    spin_unlock_irqrestore(&ns_lock, f);
    return nr;
}

int pid_to_user(int gid) { return pid_in(proc_current()->ns.pid, gid); }

int pid_from_user(int upid) {
    struct pidns *viewer = proc_current()->ns.pid;
    if (upid <= 0 || !viewer || viewer->level == 0)
        return upid;
    int gid = 0;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    for (int b = 0; b < PIDHASH && !gid; b++)
        for (struct pidlink *l = pidhash[b]; l; l = l->next)
            if (l->ns->level >= viewer->level && l->nr[viewer->level] == upid &&
                ancestor(l->ns, viewer->level) == viewer) {
                gid = l->gid;
                break;
            }
    spin_unlock_irqrestore(&ns_lock, f);
    return gid;
}

int pid_ns_chain(int gid, struct pidns *viewer, int *out, int max) {
    int vl = viewer ? viewer->level : 0, n = 0;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    struct pidlink *l = link_of(gid);
    if (!l) {
        if (vl == 0 && max > 0)
            out[n++] = gid;
    } else if (ancestor(l->ns, vl) == (vl ? viewer : ancestor(l->ns, 0))) {
        for (int lv = vl; lv <= l->ns->level && n < max; lv++)
            out[n++] = l->nr[lv];
    }
    spin_unlock_irqrestore(&ns_lock, f);
    return n;
}

struct pidns *pidns_of(struct process *p) { return p->ns.pid; }
int pidns_level(const struct pidns *ns) { return ns ? ns->level : 0; }
int pidns_reaper(const struct pidns *ns) { return ns && ns->level ? ns->reaper : 1; }
bool pidns_is_reaper(struct process *p) {
    struct pidns *ns = p->ns.pid;
    return ns && ns->level > 0 && ns->reaper == p->pid;
}
bool pidns_contains(const struct pidns *ns, struct process *p) {
    struct pidns *q = p->ns.pid;
    while (q && q->level > ns->level)
        q = q->parent;
    return q == ns;
}

/* ---- user ids ---- */

static bool map_down(const struct idmap *m, uint32_t id, uint32_t *out) {
    for (int i = 0; i < m->n; i++)
        if (id >= m->e[i].lower && id - m->e[i].lower < m->e[i].count) {
            *out = m->e[i].first + (id - m->e[i].lower);
            return true;
        }
    return false;
}

static bool map_up(const struct idmap *m, uint32_t id, uint32_t *out) {
    for (int i = 0; i < m->n; i++)
        if (id >= m->e[i].first && id - m->e[i].first < m->e[i].count) {
            *out = m->e[i].lower + (id - m->e[i].first);
            return true;
        }
    return false;
}

/* The global id `kid` as namespace `ns` names it. */
static bool to_ns(const struct userns *ns, uint32_t kid, bool gid, uint32_t *out) {
    if (!ns || ns->level == 0) {
        *out = kid;
        return true;
    }
    uint32_t up;
    if (!to_ns(ns->parent, kid, gid, &up))
        return false;
    return map_down(gid ? &ns->gid : &ns->uid, up, out);
}

static bool from_ns(const struct userns *ns, uint32_t id, bool gid, uint32_t *out) {
    for (; ns && ns->level > 0; ns = ns->parent)
        if (!map_up(gid ? &ns->gid : &ns->uid, id, &id))
            return false;
    *out = id;
    return true;
}

uint32_t uid_to_user(uint32_t kuid) {
    uint32_t v;
    return to_ns(proc_current()->ns.user, kuid, false, &v) ? v : OVERFLOW_ID;
}

uint32_t gid_to_user(uint32_t kgid) {
    uint32_t v;
    return to_ns(proc_current()->ns.user, kgid, true, &v) ? v : OVERFLOW_ID;
}

bool uid_from_user(uint32_t uid, uint32_t *kuid) { return from_ns(proc_current()->ns.user, uid, false, kuid); }
bool gid_from_user(uint32_t gid, uint32_t *kgid) { return from_ns(proc_current()->ns.user, gid, true, kgid); }

bool proc_has_cap(int cap); /* kernel/linux/sys_proc.c */
bool ns_capable(void) {
    return proc_has_cap(21); /* CAP_SYS_ADMIN, in its own user namespace */
}

int userns_level(const struct userns *ns) { return ns ? ns->level : 0; }

/* ---- network and host names ---- */

bool netns_is_initial(const struct netns *ns) { return !ns || ns->initial; }
struct netns *netns_current(void) { return proc_current()->ns.net; }
char *uts_hostname(void) {
    struct utsns *u = proc_current()->ns.uts;
    return (u ? u : &first_uts)->host;
}
char *uts_domainname(void) {
    struct utsns *u = proc_current()->ns.uts;
    return (u ? u : &first_uts)->domain;
}

/* ---- clone and unshare ---- */

bool ns_flags_valid(uint64_t flags) { return !(flags & CLONE_NS_ALL & ~CLONE_NS_ALL); }

/* Replaces the namespaces of `s` that `flags` asks to be new. `owner` is
 * the creator's global uid (for a new user namespace). */
static int new_namespaces(struct process *p, struct ns_set *s, uint64_t flags, bool for_child) {
    if (flags & CLONE_NEWUSER) {
        struct userns *u;
        int r = userns_new(s->user, p->euid, &u);
        if (r < 0)
            return r;
        userns_put(s->user);
        s->user = u;
        p->cap_eff_off = p->cap_perm_off = 0; /* all of them, in the new namespace */
    }
    if (flags & CLONE_NEWPID) {
        struct pidns *n = pidns_new(s->pid_children);
        if (!n)
            return -ENOMEM;
        pidns_put(s->pid_children);
        s->pid_children = n;
        if (for_child) { /* the child itself is its first process */
            pidns_put(s->pid);
            s->pid = ref(n);
        }
    }
    if (flags & CLONE_NEWNET) {
        struct netns *n = netns_new();
        if (!n)
            return -ENOMEM;
        netns_put(s->net);
        s->net = n;
    }
    if (flags & CLONE_NEWUTS) {
        struct utsns *n = utsns_copy(s->uts ? s->uts : &first_uts);
        if (!n)
            return -ENOMEM;
        plain_put(s->uts, &first_uts);
        s->uts = n;
    }
    struct nsobj **objs[3] = {&s->ipc, &s->cgroup, &s->time};
    uint64_t bits[3] = {CLONE_NEWIPC, CLONE_NEWCGROUP, CLONE_NEWTIME};
    struct nsobj *firsts[3] = {&first_ipc, &first_cgroup, &first_time};
    for (int i = 0; i < 3; i++)
        if (flags & bits[i]) {
            struct nsobj *n = nsobj_new();
            if (!n)
                return -ENOMEM;
            plain_put(*objs[i], firsts[i]);
            *objs[i] = n;
        }
    return 0;
}

int ns_clone(struct process *parent, struct process *child, uint64_t flags) {
    ns_set_copy(&child->ns, &parent->ns);
    /* the child's pid lives where the parent's children's go */
    pidns_put(child->ns.pid);
    child->ns.pid = ref(child->ns.pid_children);
    int r = new_namespaces(child, &child->ns, flags, true);
    if (r < 0)
        return r;
    if (flags & CLONE_NEWNS) {
        struct mntns *m = mntns_copy(child->mntns, &child->root, &child->cwd);
        if (!m)
            return -ENOMEM;
        mntns_put(child->mntns);
        child->mntns = m;
    }
    r = pid_register(child->ns.pid, child->pid);
    return r < 0 ? r : 0;
}

SYSCALL(unshare) {
    UNUSED_ARGS();
    uint64_t flags = a1;
    struct process *p = proc_current();
    if (flags & ~(CLONE_NS_ALL | CLONE_FS | CLONE_FILES | CLONE_SYSVSEM | CLONE_VM | CLONE_SIGHAND | CLONE_THREAD))
        return -EINVAL;
    if ((flags & (CLONE_NEWUSER | CLONE_NEWNS)) && p->nthreads > 1)
        return -EINVAL; /* as Linux: only a single-threaded process can change these */
    struct ns_set s;
    ns_set_copy(&s, &p->ns);
    int r = new_namespaces(p, &s, flags, false);
    if (r < 0) {
        ns_set_put(&s);
        return r;
    }
    struct mntns *m = NULL;
    struct path root = {0}, cwd = {0};
    if (flags & CLONE_NEWNS) {
        fs_context(&root, &cwd);
        m = mntns_copy(p->mntns, &root, &cwd);
        if (!m) {
            path_put(&root);
            path_put(&cwd);
            ns_set_put(&s);
            return -ENOMEM;
        }
    }
    uint64_t lf = proc_table_lock();
    struct ns_set old = p->ns;
    p->ns = s;
    struct mntns *oldm = NULL;
    struct path oldroot = {0}, oldcwd = {0};
    if (m) {
        oldm = p->mntns;
        p->mntns = m;
        oldroot = p->root;
        oldcwd = p->cwd;
        p->root = root;
        p->cwd = cwd;
    }
    proc_table_unlock(lf);
    ns_set_put(&old);
    path_put(&oldroot);
    path_put(&oldcwd);
    mntns_put(oldm);
    if (flags & CLONE_FILES) { /* a private copy of a shared descriptor table */
        struct fdtable *t = fdtable_dup(p->fds);
        if (t) {
            struct fdtable *was = p->fds;
            p->fds = t;
            fdtable_put(was);
        }
    }
    return 0;
}

/* ---- /proc/<pid>/ns ---- */

enum ns_type { NS_MNT, NS_PID, NS_PID_CHILDREN, NS_USER, NS_NET, NS_UTS, NS_IPC, NS_CGROUP, NS_TIME,
               NS_TIME_CHILDREN, NS_NTYPES };
static const char *const ns_names[NS_NTYPES] = {"mnt", "pid", "pid_for_children", "user", "net", "uts", "ipc",
                                                 "cgroup", "time", "time_for_children"};
static const char *const ns_kinds[NS_NTYPES] = {"mnt", "pid", "pid", "user", "net", "uts", "ipc",
                                                 "cgroup", "time", "time"};
static const uint64_t ns_clone_bits[NS_NTYPES] = {CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWPID, CLONE_NEWUSER,
                                                   CLONE_NEWNET, CLONE_NEWUTS, CLONE_NEWIPC, CLONE_NEWCGROUP,
                                                   CLONE_NEWTIME, CLONE_NEWTIME};

struct nsref {
    enum ns_type type;
    void *obj; /* referenced */
};

static int ns_type_of(const char *name) {
    for (int i = 0; i < NS_NTYPES; i++)
        if (!strcmp(name, ns_names[i]))
            return i;
    return -1;
}

/* proc_table_lock held: the object (unreferenced) and its id. */
static void *ns_object(struct process *p, enum ns_type t, uint64_t *id) {
    switch (t) {
    case NS_MNT: *id = mntns_id(p->mntns); return p->mntns;
    case NS_PID: *id = p->ns.pid->level ? p->ns.pid->id : first_pid.id; return p->ns.pid;
    case NS_PID_CHILDREN: *id = p->ns.pid_children->level ? p->ns.pid_children->id : first_pid.id; return p->ns.pid_children;
    case NS_USER: *id = p->ns.user->level ? p->ns.user->id : first_user.id; return p->ns.user;
    case NS_NET: *id = p->ns.net->id; return p->ns.net;
    case NS_UTS: *id = p->ns.uts->id; return p->ns.uts;
    case NS_IPC: *id = p->ns.ipc->id; return p->ns.ipc;
    case NS_CGROUP: *id = p->ns.cgroup->id; return p->ns.cgroup;
    case NS_TIME:
    case NS_TIME_CHILDREN: *id = p->ns.time->id; return p->ns.time;
    default: return NULL;
    }
}

static void nsref_put(struct nsref *r) {
    switch (r->type) {
    case NS_MNT: mntns_put(r->obj); break;
    case NS_PID:
    case NS_PID_CHILDREN: pidns_put(r->obj); break;
    case NS_USER: userns_put(r->obj); break;
    case NS_NET: netns_put(r->obj); break;
    case NS_UTS: plain_put(r->obj, &first_uts); break;
    case NS_IPC: plain_put(r->obj, &first_ipc); break;
    case NS_CGROUP: plain_put(r->obj, &first_cgroup); break;
    default: plain_put(r->obj, &first_time); break;
    }
}

static void nsfs_release(struct vnode *v) {
    struct nsref *r = v->data;
    if (r) {
        nsref_put(r);
        kfree(r);
    }
}

static const struct vnode_ops nsfs_ops = {.release = nsfs_release};

int ns_type_by_name(const char *name) { return ns_type_of(name); }

/* proc_table_lock held: a reference on `p`'s namespace of `type`. */
void *ns_ref_locked(struct process *p, int type, uint64_t *id) {
    if (type < 0 || type >= NS_NTYPES || !p->ns.pid)
        return NULL;
    void *obj = ns_object(p, (enum ns_type)type, id);
    if (obj) {
        if (type == NS_MNT)
            mntns_get(obj);
        else
            ref(obj);
    }
    return obj;
}

int ns_text(int type, uint64_t id, char *buf, size_t size) {
    return ksnprintf(buf, size, "%s:[%lu]", ns_kinds[type], id);
}

void ns_unref(int type, void *obj) {
    struct nsref r = {(enum ns_type)type, obj};
    nsref_put(&r);
}

/* An openable object for the namespace (takes the reference on `obj`). */
int ns_make_vnode(int type, void *obj, uint64_t id, struct vnode **out) {
    struct nsref *r = kzalloc(sizeof *r);
    struct vnode *v = r ? vnode_alloc(VN_REG, 0444, &nsfs_ops) : NULL;
    if (!v) {
        kfree(r);
        ns_unref(type, obj);
        return -ENOMEM;
    }
    r->type = (enum ns_type)type;
    r->obj = obj;
    v->data = r;
    v->ino = id; /* how tools tell namespaces apart: stat's st_ino */
    *out = v;
    return 0;
}

struct userns *userns_ref_locked(struct process *p) { return p->ns.user ? ref(p->ns.user) : NULL; }
void userns_release(struct userns *u) { userns_put(u); }

void mntns_root(struct mntns *ns, struct path *out);

SYSCALL(setns) {
    UNUSED_ARGS();
    struct file *f = fd_get((int)a1);
    if (!f)
        return -EBADF;
    struct nsref *r = f->vnode->ops == &nsfs_ops ? f->vnode->data : NULL;
    int rc = 0;
    if (!r)
        rc = -EINVAL;
    else if (a2 && !(a2 & ns_clone_bits[r->type]))
        rc = -EINVAL;
    if (rc) {
        file_put(f);
        return rc;
    }
    struct process *p = proc_current();
    struct path root = {0}, cwd = {0}, oldroot = {0}, oldcwd = {0};
    if (r->type == NS_MNT) {
        mntns_root(r->obj, &root);
        cwd = root;
        path_get(&cwd);
    }
    uint64_t lf = proc_table_lock();
    void *old = NULL;
    enum ns_type t = r->type;
    switch (t) {
    case NS_MNT:
        old = p->mntns;
        p->mntns = mntns_get(r->obj);
        oldroot = p->root;
        oldcwd = p->cwd;
        p->root = root;
        p->cwd = cwd;
        break;
    case NS_PID:
    case NS_PID_CHILDREN: old = p->ns.pid_children; p->ns.pid_children = ref(r->obj); t = NS_PID; break;
    case NS_USER: old = p->ns.user; p->ns.user = ref(r->obj); break;
    case NS_NET: old = p->ns.net; p->ns.net = ref(r->obj); break;
    case NS_UTS: old = p->ns.uts; p->ns.uts = ref(r->obj); break;
    case NS_IPC: old = p->ns.ipc; p->ns.ipc = ref(r->obj); break;
    case NS_CGROUP: old = p->ns.cgroup; p->ns.cgroup = ref(r->obj); break;
    default: old = p->ns.time; p->ns.time = ref(r->obj); break;
    }
    proc_table_unlock(lf);
    struct nsref was = {t, old};
    nsref_put(&was);
    path_put(&oldroot);
    path_put(&oldcwd);
    file_put(f);
    return 0;
}

/* ---- /proc/<pid>/uid_map, gid_map, setgroups ---- */

void ns_show_map(struct userns *u, int which, struct sbuf *b) {
    if (which == 2) {
        sb_printf(b, "%s\n", u && u->setgroups_deny ? "deny" : "allow");
        return;
    }
    if (!u || u->level == 0) {
        sb_printf(b, "%10u %10u %10u\n", 0u, 0u, 4294967295u);
        return;
    }
    const struct idmap *m = which ? &u->gid : &u->uid;
    for (int i = 0; i < m->n; i++)
        sb_printf(b, "%10u %10u %10u\n", m->e[i].first, m->e[i].lower, m->e[i].count);
}

static const char *parse_u32(const char *s, const char *end, uint32_t *out) {
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (s >= end || *s < '0' || *s > '9')
        return NULL;
    uint64_t v = 0;
    while (s < end && *s >= '0' && *s <= '9')
        v = v * 10 + (uint64_t)(*s++ - '0');
    if (v > 0xffffffffULL)
        return NULL;
    *out = (uint32_t)v;
    return s;
}

int64_t ns_store_map(struct userns *u, int which, const char *buf, size_t len) {
    if (!u || u->level == 0)
        return -EPERM;
    if (which == 2) {
        if (u->gid.n)
            return -EPERM; /* only before the gid map */
        if (len >= 4 && !strncmp(buf, "deny", 4))
            u->setgroups_deny = true;
        else if (len >= 5 && !strncmp(buf, "allow", 5))
            u->setgroups_deny = false;
        else
            return -EINVAL;
        return (int64_t)len;
    }
    struct idmap m = {0};
    const char *s = buf, *end = buf + len;
    while (s < end) {
        while (s < end && (*s == '\n' || *s == ' '))
            s++;
        if (s >= end)
            break;
        if (m.n == 5)
            return -EINVAL;
        uint32_t first, lower, count;
        if (!(s = parse_u32(s, end, &first)) || !(s = parse_u32(s, end, &lower)) || !(s = parse_u32(s, end, &count)) ||
            !count)
            return -EINVAL;
        /* `lower` is named in the parent namespace: as a global id there */
        m.e[m.n].first = first;
        m.e[m.n].lower = lower;
        m.e[m.n].count = count;
        m.n++;
        while (s < end && *s != '\n')
            s++;
    }
    if (!m.n)
        return -EINVAL;
    struct idmap *target = which ? &u->gid : &u->uid;
    uint64_t f = spin_lock_irqsave(&ns_lock);
    bool set = target->n != 0;
    if (!set)
        *target = m;
    spin_unlock_irqrestore(&ns_lock, f);
    return set ? -EPERM : (int64_t)len;
}
