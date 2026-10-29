#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Namespaces (proc/ns.c): what a process sees as process ids, user ids,
 * the network and the host name, so sandboxes (bubblewrap, Flatpak,
 * Chromium) can give an app a private world. Mount namespaces are in
 * fs/mount.c.
 *
 * Inside the kernel every process, thread, user and group keeps its one
 * global id; the namespaces only change how ids are seen at the system
 * call boundary (getpid, kill, wait4, stat, /proc...). */

struct process;
struct pidns;
struct userns;
struct netns;
struct utsns;
struct nsobj; /* ipc, cgroup and time namespaces: nothing in them yet but their identity */

#define PIDNS_MAX 8   /* nesting depth */
#define OVERFLOW_ID 65534 /* what an id with no name in a user namespace reads as */

/* The namespaces a process lives in. All referenced. */
struct ns_set {
    struct pidns *pid;          /* where its own pid is */
    struct pidns *pid_children; /* where its children's go (unshare(CLONE_NEWPID) changes only this) */
    struct userns *user;
    struct netns *net;
    struct utsns *uts;
    struct nsobj *ipc, *cgroup, *time;
};

void ns_init(struct ns_set *first);
void ns_set_copy(struct ns_set *dst, const struct ns_set *src); /* references */
void ns_set_put(struct ns_set *s);
/* clone(): the child's namespaces from the parent's and the CLONE_NEW* flags. */
int ns_clone(struct process *parent, struct process *child, uint64_t flags);
bool ns_flags_valid(uint64_t flags);

/* Process ids. A new id (process or thread) in `ns` gets a number at each
 * level; the reaper is pid 1 of a namespace. */
int pid_register(struct pidns *ns, int gid);
void pid_unregister(int gid);
/* `gid` as the calling process sees it (0: not visible from there). */
int pid_to_user(int gid);
int pid_in(struct pidns *viewer, int gid);
/* A number the caller gave, as a global id (0 if none). */
int pid_from_user(int upid);
struct pidns *pidns_of(struct process *p);
int pidns_level(const struct pidns *ns);
/* The pid-1 process of the caller's (or p's) namespace, as a global id (0: none). */
int pidns_reaper(const struct pidns *ns);
bool pidns_is_reaper(struct process *p);
/* Is `p` in `ns` or below it? */
bool pidns_contains(const struct pidns *ns, struct process *p);
/* The numbers of `gid` from its own namespace up to the viewer's ("NSpid"). */
int pid_ns_chain(int gid, struct pidns *viewer, int *out, int max);

/* User ids: global (kernel) ids <-> what the caller's user namespace calls them. */
uint32_t uid_to_user(uint32_t kuid);
uint32_t gid_to_user(uint32_t kgid);
bool uid_from_user(uint32_t uid, uint32_t *kuid);
bool gid_from_user(uint32_t gid, uint32_t *kgid);
/* Has the caller every capability (root, or the owner of its user namespace)? */
bool ns_capable(void);
int userns_level(const struct userns *ns);
/* user.max_user_namespaces of the caller's user namespace (set: its root only). */
int userns_max_get(void);
int userns_max_set(int v);

/* Networks: the first one has the real interfaces; any other has nothing
 * but its own loopback (and no IP traffic at all, here). */
bool netns_is_initial(const struct netns *ns);
struct netns *netns_current(void);
struct netns *netns_get(struct netns *ns);
void netns_put(struct netns *ns);

/* Host and domain name of the caller's UTS namespace. */
char *uts_hostname(void);
char *uts_domainname(void);

/* /proc/<pid>/ns/<name>: "mnt:[4026531840]", and an object to open for setns.
 * ns_ref_locked runs under proc_table_lock (it only takes a reference). */
struct vnode;
int ns_type_by_name(const char *name);
void *ns_ref_locked(struct process *p, int type, uint64_t *id);
int ns_text(int type, uint64_t id, char *buf, size_t size);
void ns_unref(int type, void *obj);
int ns_make_vnode(int type, void *obj, uint64_t id, struct vnode **out);
/* /proc/<pid>/uid_map, gid_map (which 0, 1) and setgroups (2) */
struct sbuf;
struct userns *userns_ref_locked(struct process *p);
void userns_release(struct userns *u);
void ns_show_map(struct userns *u, int which, struct sbuf *b);
int64_t ns_store_map(struct userns *u, int which, const char *buf, size_t len);
