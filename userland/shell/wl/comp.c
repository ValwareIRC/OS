#define _GNU_SOURCE
#include "wlint.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The interfaces apps use to show windows: surfaces with shared-memory
 * buffers, subsurfaces, xdg-shell toplevels and popups, the seat (pointer and
 * keyboard) and one output. Each committed frame is copied (so the buffer
 * goes straight back to the app), and a window's view is its surface with
 * its subsurfaces drawn in, in their order, cropped to the window geometry
 * (so client-drawn shadows don't count). Decorations are the shell's unless
 * the app draws its own: the person's design frames every window. */

void wl_flush_all(void);

#define MAX_VIEWS 32

/* ---- shared memory pools and buffers ---- */

struct pool {
    int refs;
    uint8_t *map;
    size_t size;
    int fd;
};

struct buffer {
    struct pool *pool;
    int32_t offset, w, h, stride;
    uint32_t format;
};

static void pool_unref(struct pool *p) {
    if (--p->refs > 0)
        return;
    if (p->map && p->map != MAP_FAILED)
        munmap(p->map, p->size);
    close(p->fd);
    free(p);
}

static void buffer_destroy(wl_obj *o) {
    struct buffer *b = o->data;
    if (b) {
        pool_unref(b->pool);
        free(b);
    }
}

static void pool_destroy(wl_obj *o) {
    if (o->data)
        pool_unref(o->data);
}

static void shm_pool_req(wl_obj *o, int op, union wl_arg *a) {
    struct pool *p = o->data;
    if (op == WL_SHM_POOL_CREATE_BUFFER) {
        int32_t off = a[1].i, w = a[2].i, h = a[3].i, stride = a[4].i;
        if (w <= 0 || h <= 0 || stride < w * 4 || off < 0 || (size_t)off + (size_t)stride * (size_t)h > p->size) {
            wl_post_error(o, WL_SHM_ERROR_INVALID_STRIDE, "buffer outside its pool");
            return;
        }
        struct buffer *b = calloc(1, sizeof *b);
        *b = (struct buffer){p, off, w, h, stride, a[5].u};
        p->refs++;
        wl_obj *bo = wl_obj_new(o->client, a[0].u, &wl_buffer_iface, 1, b);
        if (bo)
            bo->destroy = buffer_destroy;
    } else if (op == WL_SHM_POOL_RESIZE) {
        size_t ns = (size_t)a[0].i;
        if (ns < p->size)
            return;
        uint8_t *m = mmap(NULL, ns, PROT_READ, MAP_SHARED, p->fd, 0);
        if (m == MAP_FAILED)
            return;
        munmap(p->map, p->size);
        p->map = m, p->size = ns;
    } else if (op == WL_SHM_POOL_DESTROY) {
        wl_obj_destroy(o);
    }
}

static void shm_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_SHM_CREATE_POOL) {
        int fd = a[1].h;
        size_t size = (size_t)a[2].i;
        uint8_t *m = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            close(fd);
            wl_post_error(o, WL_SHM_ERROR_INVALID_FD, "can't map the pool");
            return;
        }
        struct pool *p = calloc(1, sizeof *p);
        *p = (struct pool){1, m, size, fd};
        wl_obj *po = wl_obj_new(o->client, a[0].u, &wl_shm_pool_iface, 1, p);
        if (po)
            po->destroy = pool_destroy;
    } else if (op == WL_SHM_RELEASE) {
        wl_obj_destroy(o);
    }
}

static void bind_shm(wl_client *c, uint32_t id, int v) {
    wl_obj *o = wl_obj_new(c, id, &wl_shm_iface, v, NULL);
    if (!o)
        return;
    wl_send(o, WL_SHM_EV_FORMAT, WL_SHM_FORMAT_ARGB8888);
    wl_send(o, WL_SHM_EV_FORMAT, WL_SHM_FORMAT_XRGB8888);
}

static void buffer_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    if (op == WL_BUFFER_DESTROY)
        wl_obj_destroy(o);
}

/* ---- surfaces ---- */

enum role { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_CURSOR, ROLE_SUB };

struct wl_surf {
    wl_obj *obj;
    enum role role;
    wl_obj *pending_buffer;
    bool attached; /* attach called since the last commit (maybe with NULL) */
    wl_obj *frames[16];
    int nframes;
    wl_obj *done_frames[16]; /* committed: answered on the next shown frame */
    int ndone;
    wl_obj *xdg, *toplevel, *popup, *deco; /* deco: its xdg-decoration object, if it made one */
    bool configured, acked;
    uint32_t last_serial;
    int gx, gy, gw, gh; /* window geometry inside the buffer; gw 0: whole buffer */
    wl_view *view;
    /* what it shows: a copy of its last buffer (premultiplied ARGB) */
    uint32_t *px;
    int pw, ph;
    /* as a subsurface (Firefox and GTK draw a window's content into one, video players their picture): its
     * parent, where it sits on it, and whether its commits wait for the parent's (synchronized, the default) */
    struct wl_surf *parent;
    wl_obj *sub;
    int sx, sy, pend_sx, pend_sy;
    bool pend_pos, desync, under; /* under: placed below its parent */
    uint32_t *cache_px;           /* synchronized: committed, shown at the parent's next commit */
    int cache_w, cache_h;
    bool cached;
    struct wl_surf *kids[16]; /* its subsurfaces, bottom first */
    int nkids;
};

static wl_view *views[MAX_VIEWS];
static int nviews;
static int next_view_id = 1;

int wls_view_count(void) { return nviews; }
wl_view *wls_view(int i) { return i >= 0 && i < nviews ? views[i] : NULL; }

static wl_view *view_new(struct wl_surf *s) {
    if (nviews >= MAX_VIEWS)
        return NULL;
    wl_view *v = calloc(1, sizeof *v);
    v->id = next_view_id++;
    v->surf = s;
    views[nviews++] = v;
    return v;
}

static void view_free(wl_view *v) {
    if (!v)
        return;
    if (v->mapped)
        wl_host_view_unmapped(v);
    for (int i = 0; i < nviews; i++)
        if (views[i]->parent == v)
            views[i]->parent = NULL;
    for (int i = 0; i < nviews; i++)
        if (views[i] == v) {
            memmove(views + i, views + i + 1, sizeof views[0] * (size_t)(nviews - i - 1));
            nviews--;
            break;
        }
    free(v->px);
    free(v);
}

static wl_obj *pointer_focus, *keyboard_focus; /* wl_surface objects */
static void unlink_sub(struct wl_surf *k);

static void surface_destroy(wl_obj *o) {
    struct wl_surf *s = o->data;
    if (!s)
        return;
    if (pointer_focus == o)
        pointer_focus = NULL;
    if (keyboard_focus == o)
        keyboard_focus = NULL;
    if (s->view) {
        s->view->surf = NULL;
        view_free(s->view);
    }
    for (int i = 0; i < s->nframes; i++)
        s->frames[i]->data = NULL;
    for (int i = 0; i < s->ndone; i++)
        s->done_frames[i]->data = NULL;
    if (s->xdg)
        s->xdg->data = NULL;
    if (s->toplevel)
        s->toplevel->data = NULL;
    if (s->popup)
        s->popup->data = NULL;
    if (s->deco)
        s->deco->data = NULL;
    if (s->sub)
        s->sub->data = NULL;
    unlink_sub(s);
    for (int i = 0; i < s->nkids; i++)
        s->kids[i]->parent = NULL;
    free(s->px);
    free(s->cache_px);
    free(s);
}

/* A surface's xdg-surface, toplevel, popup or decoration object freed (asked for, or its app gone, when objects
 * go in any order): the surface no longer points at it. */
static void role_obj_gone(wl_obj *o) {
    struct wl_surf *s = o->data;
    if (!s)
        return;
    if (s->xdg == o)
        s->xdg = NULL;
    if (s->toplevel == o)
        s->toplevel = NULL;
    if (s->popup == o)
        s->popup = NULL;
    if (s->deco == o)
        s->deco = NULL;
}

static void callback_destroy(wl_obj *o) {
    struct wl_surf *s = o->data;
    if (!s)
        return;
    for (int i = 0; i < s->nframes; i++)
        if (s->frames[i] == o) {
            s->frames[i] = s->frames[--s->nframes];
            break;
        }
    for (int i = 0; i < s->ndone; i++)
        if (s->done_frames[i] == o) {
            s->done_frames[i] = s->done_frames[--s->ndone];
            break;
        }
}

/* A copy of the committed buffer: XRGB made opaque, ARGB kept as it is (premultiplied). */
static bool copy_buffer(struct buffer *b, uint32_t **px, int *w, int *h) {
    if (!b || !b->pool->map)
        return false;
    if (b->w != *w || b->h != *h || !*px) {
        free(*px);
        *px = malloc((size_t)b->w * (size_t)b->h * 4);
        *w = b->w, *h = b->h;
        if (!*px) {
            *w = *h = 0;
            return false;
        }
    }
    bool opaque = b->format == WL_SHM_FORMAT_XRGB8888;
    for (int y = 0; y < b->h; y++) {
        const uint32_t *src = (const uint32_t *)(b->pool->map + b->offset + (size_t)y * (size_t)b->stride);
        uint32_t *dst = *px + (size_t)y * (size_t)b->w;
        if (opaque)
            for (int x = 0; x < b->w; x++)
                dst[x] = src[x] | 0xff000000u;
        else
            memcpy(dst, src, (size_t)b->w * 4);
    }
    return true;
}

static struct wl_surf *surf_root(struct wl_surf *s) {
    for (int i = 0; s && s->parent && i < 32; i++)
        s = s->parent;
    return s;
}

/* Draws `s` and its subsurfaces (those below it first) into `dst`, its top-left at (ox, oy). */
static void draw_tree(struct wl_surf *s, uint32_t *dst, int dw, int dh, int ox, int oy, int depth) {
    if (depth > 8)
        return;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) { /* the surface itself, between those below and those above */
            for (int y = 0; s->px && y < s->ph; y++) {
                int ty = oy + y;
                if (ty < 0 || ty >= dh)
                    continue;
                const uint32_t *src = s->px + (size_t)y * (size_t)s->pw;
                uint32_t *row = dst + (size_t)ty * (size_t)dw;
                for (int x = 0; x < s->pw; x++) {
                    int tx = ox + x;
                    if (tx < 0 || tx >= dw)
                        continue;
                    uint32_t c = src[x], al = c >> 24;
                    if (al == 255) {
                        row[tx] = c;
                    } else if (al) { /* premultiplied: c + d * (1 - a) */
                        uint32_t d = row[tx], k = 255 - al;
                        uint32_t rb = ((d & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu;
                        uint32_t ag = (((d >> 8) & 0x00ff00ffu) * k) & 0xff00ff00u;
                        row[tx] = c + rb + ag;
                    }
                }
            }
            continue;
        }
        for (int i = 0; i < s->nkids; i++)
            if (s->kids[i]->under == (pass == 0))
                draw_tree(s->kids[i], dst, dw, dh, ox + s->kids[i]->sx, oy + s->kids[i]->sy, depth + 1);
    }
}

/* The window's view made again from its surfaces: only the window geometry. */
static void compose(struct wl_surf *root) {
    wl_view *v = root ? root->view : NULL;
    if (!v || !root->px)
        return;
    int x0 = 0, y0 = 0, w = root->pw, h = root->ph;
    if (root->gw > 0 && root->gh > 0) {
        x0 = root->gx, y0 = root->gy, w = root->gw, h = root->gh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + w > root->pw) w = root->pw - x0;
        if (y0 + h > root->ph) h = root->ph - y0;
        if (w <= 0 || h <= 0)
            x0 = y0 = 0, w = root->pw, h = root->ph;
    }
    if (w != v->w || h != v->h || !v->px) {
        free(v->px);
        v->px = malloc((size_t)w * (size_t)h * 4);
        v->w = w, v->h = h;
        if (!v->px) {
            v->w = v->h = 0;
            return;
        }
    }
    if (!root->nkids) { /* the usual window: its buffer, cropped */
        for (int y = 0; y < h; y++)
            memcpy(v->px + (size_t)y * (size_t)w, root->px + (size_t)(y + y0) * (size_t)root->pw + x0, (size_t)w * 4);
    } else {
        memset(v->px, 0, (size_t)w * (size_t)h * 4);
        draw_tree(root, v->px, w, h, -x0, -y0, 0);
    }
    v->version++;
    if (!v->mapped) {
        v->mapped = true;
        wl_host_view_mapped(v);
    }
}

/* A subsurface is synchronized when it or any parent above it is. */
static bool sub_synced(struct wl_surf *s) {
    for (int i = 0; s && s->parent && i < 32; i++, s = s->parent)
        if (!s->desync)
            return true;
    return false;
}

static void show_cached(struct wl_surf *k) {
    uint32_t *t = k->px;
    k->px = k->cache_px, k->pw = k->cache_w, k->ph = k->cache_h;
    k->cache_px = t, k->cached = false;
    if (!k->px)
        k->pw = k->ph = 0;
    k->cache_w = k->cache_h = 0;
    free(k->cache_px);
    k->cache_px = NULL;
}

/* The parent committed: its subsurfaces' new positions and synchronized states take effect. */
static void apply_kids(struct wl_surf *s, int depth) {
    if (depth > 8)
        return;
    for (int i = 0; i < s->nkids; i++) {
        struct wl_surf *k = s->kids[i];
        if (k->pend_pos)
            k->sx = k->pend_sx, k->sy = k->pend_sy, k->pend_pos = false;
        if (k->cached)
            show_cached(k);
        apply_kids(k, depth + 1);
    }
}

static void unlink_sub(struct wl_surf *k) {
    struct wl_surf *p = k->parent;
    if (!p)
        return;
    for (int i = 0; i < p->nkids; i++)
        if (p->kids[i] == k) {
            memmove(p->kids + i, p->kids + i + 1, sizeof p->kids[0] * (size_t)(p->nkids - i - 1));
            p->nkids--;
            break;
        }
    k->parent = NULL;
    compose(surf_root(p));
}

static void send_configure(struct wl_surf *s, int w, int h, bool activated) {
    if (s->toplevel) {
        uint32_t states[8];
        int n = 0;
        if (activated)
            states[n++] = XDG_TOPLEVEL_STATE_ACTIVATED;
        unsigned st = s->view ? s->view->wm_state : 0;
        if (st & WLS_STATE_MAXIMIZED) states[n++] = XDG_TOPLEVEL_STATE_MAXIMIZED;
        if (st & WLS_STATE_RESIZING) states[n++] = XDG_TOPLEVEL_STATE_RESIZING;
        if (st & WLS_STATE_TILED_LEFT) states[n++] = XDG_TOPLEVEL_STATE_TILED_LEFT;
        if (st & WLS_STATE_TILED_RIGHT) states[n++] = XDG_TOPLEVEL_STATE_TILED_RIGHT;
        if (st & WLS_STATE_TILED_TOP) states[n++] = XDG_TOPLEVEL_STATE_TILED_TOP;
        if (st & WLS_STATE_TILED_BOTTOM) states[n++] = XDG_TOPLEVEL_STATE_TILED_BOTTOM;
        wl_send(s->toplevel, XDG_TOPLEVEL_EV_CONFIGURE, w, h, (const void *)states, (uint32_t)(n * 4));
    }
    if (s->xdg) {
        s->last_serial = wl_next_serial();
        wl_send(s->xdg, XDG_SURFACE_EV_CONFIGURE, s->last_serial);
    }
    s->configured = true;
}

static void surface_req(wl_obj *o, int op, union wl_arg *a) {
    struct wl_surf *s = o->data;
    if (!s)
        return;
    switch (op) {
    case WL_SURFACE_DESTROY:
        wl_obj_destroy(o);
        break;
    case WL_SURFACE_ATTACH:
        s->pending_buffer = a[0].u ? wl_obj_get(o->client, a[0].u) : NULL;
        s->attached = true;
        break;
    case WL_SURFACE_FRAME: {
        wl_obj *cb = wl_obj_new(o->client, a[0].u, &wl_callback_iface, 1, s);
        if (cb) {
            cb->destroy = callback_destroy;
            if (s->nframes < 16)
                s->frames[s->nframes++] = cb;
        }
        break;
    }
    case WL_SURFACE_COMMIT:
        if (s->xdg && !s->configured) {
            /* the first commit, before any buffer: tell it its size */
            if (s->toplevel)
                wl_host_log("wayland: client %p window: first commit, sending configure", (void *)o->client);
            send_configure(s, 0, 0, true);
            break;
        }
        bool synced = s->role == ROLE_SUB && sub_synced(s);
        if (s->attached && synced) { /* synchronized: kept until the parent commits */
            free(s->cache_px), s->cache_px = NULL, s->cache_w = s->cache_h = 0;
            if (s->pending_buffer && s->pending_buffer->data)
                copy_buffer(s->pending_buffer->data, &s->cache_px, &s->cache_w, &s->cache_h);
            if (s->pending_buffer)
                wl_send(s->pending_buffer, WL_BUFFER_EV_RELEASE);
            s->cached = true;
            s->attached = false;
            s->pending_buffer = NULL;
        } else if (s->attached) {
            if (s->pending_buffer && s->pending_buffer->data) {
                if (s->toplevel && s->view && !s->view->mapped)
                    wl_host_log("wayland: client %p window: first buffer", (void *)o->client);
                copy_buffer(s->pending_buffer->data, &s->px, &s->pw, &s->ph);
                wl_send(s->pending_buffer, WL_BUFFER_EV_RELEASE);
            } else {
                free(s->px), s->px = NULL, s->pw = s->ph = 0;
                if (s->view && s->view->mapped) {
                    s->view->mapped = false; /* a NULL buffer hides the window */
                    wl_host_view_unmapped(s->view);
                }
            }
            s->attached = false;
            s->pending_buffer = NULL;
        }
        if (s->nkids)
            apply_kids(s, 0);
        if (!synced)
            compose(surf_root(s));
        for (int i = 0; i < s->nframes && s->ndone < 16; i++)
            s->done_frames[s->ndone++] = s->frames[i];
        s->nframes = 0;
        break;
    default: /* damage, regions, transforms, scale, offset: everything is redrawn anyway */
        break;
    }
}

static void region_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    if (op == WL_REGION_DESTROY)
        wl_obj_destroy(o);
}

static void compositor_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_COMPOSITOR_CREATE_SURFACE) {
        struct wl_surf *s = calloc(1, sizeof *s);
        wl_obj *so = wl_obj_new(o->client, a[0].u, &wl_surface_iface, o->version, s);
        if (!so) {
            free(s);
            return;
        }
        s->obj = so;
        so->destroy = surface_destroy;
    } else if (op == WL_COMPOSITOR_CREATE_REGION) {
        wl_obj_new(o->client, a[0].u, &wl_region_iface, 1, NULL);
    }
}

static void bind_compositor(wl_client *c, uint32_t id, int v) { wl_obj_new(c, id, &wl_compositor_iface, v, NULL); }

/* Moves subsurface `k` right above or below `sib`: a sibling, or the parent itself. */
static void sub_place(struct wl_surf *k, struct wl_surf *sib, bool above) {
    struct wl_surf *p = k->parent;
    if (!p || sib == k)
        return;
    int at = -1;
    for (int i = 0; i < p->nkids; i++)
        if (p->kids[i] == k)
            at = i;
    if (at < 0)
        return;
    memmove(p->kids + at, p->kids + at + 1, sizeof p->kids[0] * (size_t)(p->nkids - at - 1));
    p->nkids--;
    int to = p->nkids;
    if (sib == p) { /* the parent sits between those under it and those over it */
        to = 0;
        while (to < p->nkids && p->kids[to]->under)
            to++;
        k->under = !above;
    } else {
        for (int i = 0; i < p->nkids; i++)
            if (p->kids[i] == sib)
                to = above ? i + 1 : i;
        k->under = sib->under;
    }
    memmove(p->kids + to + 1, p->kids + to, sizeof p->kids[0] * (size_t)(p->nkids - to));
    p->kids[to] = k;
    p->nkids++;
}

static void subsurface_destroyed(wl_obj *o) {
    struct wl_surf *k = o->data;
    if (k) {
        k->sub = NULL;
        unlink_sub(k);
        k->role = ROLE_NONE;
    }
}

static void subsurface_req(wl_obj *o, int op, union wl_arg *a) {
    struct wl_surf *k = o->data;
    if (op == WL_SUBSURFACE_DESTROY) {
        wl_obj_destroy(o);
        return;
    }
    if (!k)
        return;
    switch (op) {
    case WL_SUBSURFACE_SET_POSITION: /* takes effect at the parent's next commit */
        k->pend_sx = a[0].i, k->pend_sy = a[1].i, k->pend_pos = true;
        break;
    case WL_SUBSURFACE_PLACE_ABOVE:
    case WL_SUBSURFACE_PLACE_BELOW: {
        wl_obj *so = wl_obj_get(o->client, a[0].u);
        struct wl_surf *sib = so ? so->data : NULL;
        if (sib && (sib == k->parent || sib->parent == k->parent))
            sub_place(k, sib, op == WL_SUBSURFACE_PLACE_ABOVE);
        break;
    }
    case WL_SUBSURFACE_SET_SYNC: k->desync = false; break;
    case WL_SUBSURFACE_SET_DESYNC:
        k->desync = true;
        if (!sub_synced(k) && k->cached) { /* what was waiting is shown now */
            show_cached(k);
            compose(surf_root(k));
        }
        break;
    default: break;
    }
}

static void subcompositor_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_SUBCOMPOSITOR_GET_SUBSURFACE) {
        wl_obj *surf = wl_obj_get(o->client, a[1].u), *par = wl_obj_get(o->client, a[2].u);
        struct wl_surf *k = surf ? surf->data : NULL, *p = par ? par->data : NULL;
        if (!k || !p || k == p || k->parent || k->role != ROLE_NONE || surf_root(p) == k || p->nkids >= 16) {
            wl_post_error(o, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE, "that surface can't be a subsurface there");
            return;
        }
        wl_obj *so = wl_obj_new(o->client, a[0].u, &wl_subsurface_iface, 1, k);
        if (!so)
            return;
        so->destroy = subsurface_destroyed;
        k->role = ROLE_SUB, k->sub = so, k->parent = p, k->desync = false, k->under = false;
        k->sx = k->sy = 0;
        p->kids[p->nkids++] = k; /* a new one goes on top */
    } else if (op == WL_SUBCOMPOSITOR_DESTROY) {
        wl_obj_destroy(o);
    }
}
static void bind_subcompositor(wl_client *c, uint32_t id, int v) { wl_obj_new(c, id, &wl_subcompositor_iface, v, NULL); }

/* ---- xdg-shell ---- */

struct positioner {
    int w, h, ax, ay, aw, ah, ox, oy;
    uint32_t anchor, gravity;
};

static void positioner_req(wl_obj *o, int op, union wl_arg *a) {
    struct positioner *p = o->data;
    switch (op) {
    case XDG_POSITIONER_DESTROY: wl_obj_destroy(o); break;
    case XDG_POSITIONER_SET_SIZE: p->w = a[0].i, p->h = a[1].i; break;
    case XDG_POSITIONER_SET_ANCHOR_RECT: p->ax = a[0].i, p->ay = a[1].i, p->aw = a[2].i, p->ah = a[3].i; break;
    case XDG_POSITIONER_SET_ANCHOR: p->anchor = a[0].u; break;
    case XDG_POSITIONER_SET_GRAVITY: p->gravity = a[0].u; break;
    case XDG_POSITIONER_SET_OFFSET: p->ox = a[0].i, p->oy = a[1].i; break;
    default: break;
    }
}

static void free_data(wl_obj *o) { free(o->data); }

static void toplevel_req(wl_obj *o, int op, union wl_arg *a) {
    struct wl_surf *s = o->data;
    if (op == XDG_TOPLEVEL_DESTROY) {
        if (s && s->view) {
            s->view->surf = NULL;
            view_free(s->view);
            s->view = NULL;
        }
        if (s)
            s->toplevel = NULL;
        wl_obj_destroy(o);
        return;
    }
    if (!s || !s->view)
        return;
    wl_view *v = s->view;
    switch (op) {
    case XDG_TOPLEVEL_SET_TITLE: snprintf(v->title, sizeof v->title, "%s", a[0].s ? a[0].s : ""); break;
    case XDG_TOPLEVEL_SET_APP_ID: snprintf(v->app_id, sizeof v->app_id, "%s", a[0].s ? a[0].s : ""); break;
    case XDG_TOPLEVEL_SET_MIN_SIZE: v->min_w = a[0].i, v->min_h = a[1].i; break;
    case XDG_TOPLEVEL_SET_MAX_SIZE: v->max_w = a[0].i, v->max_h = a[1].i; break;
    /* the shell's window manager does these, as if done on the frame */
    case XDG_TOPLEVEL_MOVE: wl_host_view_request(v, WLS_REQ_MOVE, 0); break;
    case XDG_TOPLEVEL_RESIZE: wl_host_view_request(v, WLS_REQ_RESIZE, a[2].u); break;
    case XDG_TOPLEVEL_SET_MAXIMIZED: wl_host_view_request(v, WLS_REQ_MAXIMIZE, 0); break;
    case XDG_TOPLEVEL_UNSET_MAXIMIZED: wl_host_view_request(v, WLS_REQ_UNMAXIMIZE, 0); break;
    case XDG_TOPLEVEL_SET_MINIMIZED: wl_host_view_request(v, WLS_REQ_MINIMIZE, 0); break;
    default: break;
    }
}

static void popup_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    struct wl_surf *s = o->data;
    if (op == XDG_POPUP_DESTROY) {
        if (s && s->view) {
            s->view->surf = NULL;
            view_free(s->view);
            s->view = NULL;
        }
        if (s)
            s->popup = NULL;
        wl_obj_destroy(o);
    }
}

static void xdg_surface_req(wl_obj *o, int op, union wl_arg *a) {
    struct wl_surf *s = o->data;
    if (op == XDG_SURFACE_DESTROY) {
        if (s)
            s->xdg = NULL;
        wl_obj_destroy(o);
        return;
    }
    if (!s)
        return;
    switch (op) {
    case XDG_SURFACE_GET_TOPLEVEL: {
        s->role = ROLE_TOPLEVEL;
        s->toplevel = wl_obj_new(o->client, a[0].u, &xdg_toplevel_iface, o->version, s);
        if (s->toplevel)
            s->toplevel->destroy = role_obj_gone;
        s->view = view_new(s);
        wl_host_log("wayland: client %p window: toplevel created", (void *)o->client);
        if (s->toplevel && o->version >= 5) {
            uint32_t caps[] = {XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE, XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE};
            wl_send(s->toplevel, XDG_TOPLEVEL_EV_WM_CAPABILITIES, (const void *)caps, (uint32_t)sizeof caps);
        }
        break;
    }
    case XDG_SURFACE_GET_POPUP: {
        s->role = ROLE_POPUP;
        s->popup = wl_obj_new(o->client, a[0].u, &xdg_popup_iface, o->version, s);
        if (s->popup)
            s->popup->destroy = role_obj_gone;
        wl_obj *po = wl_obj_get(o->client, a[2].u);
        struct positioner *p = po ? po->data : NULL;
        s->view = view_new(s);
        wl_obj *parent_xdg = a[1].u ? wl_obj_get(o->client, a[1].u) : NULL;
        struct wl_surf *ps = parent_xdg ? parent_xdg->data : NULL;
        if (s->view) {
            s->view->popup = true;
            s->view->parent = ps ? ps->view : NULL;
        }
        if (s->popup && p) {
            /* place it at the anchor rectangle's bottom-left, offset */
            int x = p->ax + p->ox, y = p->ay + p->ah + p->oy;
            if (s->view) {
                s->view->min_w = x; /* popups: min_w/min_h carry the offset in the parent */
                s->view->min_h = y;
            }
            wl_send(s->popup, XDG_POPUP_EV_CONFIGURE, x, y, p->w, p->h);
        }
        break;
    }
    case XDG_SURFACE_SET_WINDOW_GEOMETRY:
        s->gx = a[0].i, s->gy = a[1].i, s->gw = a[2].i, s->gh = a[3].i;
        break;
    case XDG_SURFACE_ACK_CONFIGURE:
        if (!s->acked && s->toplevel)
            wl_host_log("wayland: client %p window: configure acknowledged", (void *)o->client);
        s->acked = true;
        break;
    default:
        break;
    }
}

static void wm_base_req(wl_obj *o, int op, union wl_arg *a) {
    switch (op) {
    case XDG_WM_BASE_DESTROY: wl_obj_destroy(o); break;
    case XDG_WM_BASE_CREATE_POSITIONER: {
        wl_obj *p = wl_obj_new(o->client, a[0].u, &xdg_positioner_iface, o->version, calloc(1, sizeof(struct positioner)));
        if (p)
            p->destroy = free_data;
        break;
    }
    case XDG_WM_BASE_GET_XDG_SURFACE: {
        wl_obj *surf = wl_obj_get(o->client, a[1].u);
        struct wl_surf *s = surf ? surf->data : NULL;
        wl_obj *x = wl_obj_new(o->client, a[0].u, &xdg_surface_iface, o->version, s);
        if (s)
            s->xdg = x;
        if (x)
            x->destroy = role_obj_gone;
        break;
    }
    default: break; /* pong */
    }
}
static void bind_wm_base(wl_client *c, uint32_t id, int v) { wl_obj_new(c, id, &xdg_wm_base_iface, v, NULL); }

/* Decorations: the shell draws a window's frame unless the app says it draws
 * its own (Prism, Chromium apps with their own title bar). Server side is
 * what the shell prefers, so it is the answer to "no preference". */
static void decoration_req(wl_obj *o, int op, union wl_arg *a) {
    struct wl_surf *s = o->data;
    if (op == ZXDG_TOPLEVEL_DECORATION_V1_DESTROY) {
        if (s)
            s->deco = NULL;
        wl_obj_destroy(o);
        return;
    }
    bool own = op == ZXDG_TOPLEVEL_DECORATION_V1_SET_MODE && a[0].u == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    if (s && s->view && s->view->client_frame != own) {
        s->view->client_frame = own;
        wl_host_log("wayland: %s draws %s frame", s->view->app_id[0] ? s->view->app_id : "a window", own ? "its own" : "no");
    }
    wl_send(o, ZXDG_TOPLEVEL_DECORATION_V1_EV_CONFIGURE,
            own ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
static void deco_manager_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == ZXDG_DECORATION_MANAGER_V1_GET_TOPLEVEL_DECORATION) {
        wl_obj *top = wl_obj_get(o->client, a[1].u);
        struct wl_surf *s = top ? top->data : NULL;
        wl_obj *d = wl_obj_new(o->client, a[0].u, &zxdg_toplevel_decoration_v1_iface, 1, s);
        if (d && s)
            s->deco = d;
        if (d)
            d->destroy = role_obj_gone;
        if (d)
            wl_send(d, ZXDG_TOPLEVEL_DECORATION_V1_EV_CONFIGURE, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else if (op == ZXDG_DECORATION_MANAGER_V1_DESTROY) {
        wl_obj_destroy(o);
    }
}
static void bind_deco(wl_client *c, uint32_t id, int v) { wl_obj_new(c, id, &zxdg_decoration_manager_v1_iface, v, NULL); }

/* ---- seat: pointer and keyboard ---- */

#include "keymap.inc" /* os_keymap: an xkb keymap (US layout), compiled at build time */

static int keymap_fd(size_t *size) {
    *size = sizeof os_keymap; /* includes the NUL, as clients expect */
    int fd = memfd_create("os-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        char path[] = "/tmp/os-keymap-XXXXXX";
        fd = mkstemp(path);
        if (fd < 0)
            return -1;
        unlink(path);
    }
    if (write(fd, os_keymap, *size) != (ssize_t)*size) {
        close(fd);
        return -1;
    }
    return fd;
}

static void input_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    /* wl_pointer.release is 1, wl_keyboard.release is 0 */
    if ((o->iface == &wl_pointer_iface && op == WL_POINTER_RELEASE) ||
        (o->iface == &wl_keyboard_iface && op == WL_KEYBOARD_RELEASE))
        wl_obj_destroy(o);
}

static void seat_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_SEAT_GET_POINTER) {
        wl_obj_new(o->client, a[0].u, &wl_pointer_iface, o->version, NULL);
    } else if (op == WL_SEAT_GET_KEYBOARD) {
        wl_obj *k = wl_obj_new(o->client, a[0].u, &wl_keyboard_iface, o->version, NULL);
        if (!k)
            return;
        size_t size;
        int fd = keymap_fd(&size);
        if (fd >= 0) {
            wl_send(k, WL_KEYBOARD_EV_KEYMAP, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
            close(fd);
        }
        wl_send(k, WL_KEYBOARD_EV_REPEAT_INFO, 30, 450);
        if (keyboard_focus && keyboard_focus->client == o->client) {
            wl_send(k, WL_KEYBOARD_EV_ENTER, wl_next_serial(), keyboard_focus, (const void *)"", 0u);
            wl_send(k, WL_KEYBOARD_EV_MODIFIERS, wl_next_serial(), 0, 0, 0, 0);
        }
    } else if (op == WL_SEAT_GET_TOUCH) {
        wl_obj_new(o->client, a[0].u, &wl_touch_iface, o->version, NULL);
    } else if (op == WL_SEAT_RELEASE) {
        wl_obj_destroy(o);
    }
}

static void bind_seat(wl_client *c, uint32_t id, int v) {
    wl_obj *o = wl_obj_new(c, id, &wl_seat_iface, v, NULL);
    if (!o)
        return;
    wl_send(o, WL_SEAT_EV_CAPABILITIES, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    wl_send(o, WL_SEAT_EV_NAME, "seat0");
}

static int screen_w = 1600, screen_h = 900;

static void output_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    if (op == WL_OUTPUT_RELEASE)
        wl_obj_destroy(o);
}
static void bind_output(wl_client *c, uint32_t id, int v) {
    wl_obj *o = wl_obj_new(c, id, &wl_output_iface, v, NULL);
    if (!o)
        return;
    wl_send(o, WL_OUTPUT_EV_GEOMETRY, 0, 0, 340, 190, WL_OUTPUT_SUBPIXEL_UNKNOWN, "Operating System", "Display",
            WL_OUTPUT_TRANSFORM_NORMAL);
    wl_send(o, WL_OUTPUT_EV_MODE, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, screen_w, screen_h, 60000);
    wl_send(o, WL_OUTPUT_EV_SCALE, 1);
    wl_send(o, WL_OUTPUT_EV_NAME, "OS-1");
    wl_send(o, WL_OUTPUT_EV_DESCRIPTION, "Operating System display");
    wl_send(o, WL_OUTPUT_EV_DONE);
}

/* The clipboard: one text for everyone, the shell's (clipboard_text(): what
 * was copied in the OS's own apps, or on the PC the VM runs on). The app
 * with the keyboard is offered it whenever it gets the keyboard or the text
 * changes; text an app copies is read from it and becomes the clipboard.
 * Drag and drop: present, so toolkits are content; nothing is dragged. */
const char *clipboard_text(void);
void clipboard_set(const char *text);

static const char *const text_types[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"};
static uint64_t offered_sig; /* the text last offered, and to whom */
static wl_obj *own_source;   /* an app's own copy, while it is the clipboard */
static int copy_fd = -1;     /* reading what it copied */
static char copy_buf[4096];
static size_t copy_len;

static uint64_t text_sig(const char *s, const void *who) {
    uint64_t h = 1469598103934665603ull ^ (uint64_t)(uintptr_t)who;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

struct offer_ctx {
    wl_client *client;
};

static void offer_to_device(wl_obj *dev, void *ctx) {
    struct offer_ctx *e = ctx;
    if (dev->client != e->client)
        return;
    wl_obj *offer = wl_obj_new_server(dev->client, &wl_data_offer_iface, dev->version, NULL);
    if (!offer)
        return;
    wl_send(dev, WL_DATA_DEVICE_EV_DATA_OFFER, offer->id);
    for (size_t i = 0; i < sizeof text_types / sizeof text_types[0]; i++)
        wl_send(offer, WL_DATA_OFFER_EV_OFFER, text_types[i]);
    wl_send(dev, WL_DATA_DEVICE_EV_SELECTION, offer);
}


/* The clipboard to the app with the keyboard, when that or the text changed. */
static void offer_clipboard(void) {
    if (!keyboard_focus || own_source)
        return;
    const char *t = clipboard_text();
    uint64_t sig = text_sig(t ? t : "", keyboard_focus->client);
    if (!t || !*t || sig == offered_sig)
        return;
    offered_sig = sig;
    struct offer_ctx e = {keyboard_focus->client};
    wl_each(&wl_data_device_iface, offer_to_device, &e);
}

/* What an app copied, read as it arrives. */
static void read_copy(void) {
    if (copy_fd < 0)
        return;
    for (;;) {
        ssize_t n = read(copy_fd, copy_buf + copy_len, sizeof copy_buf - 1 - copy_len);
        if (n > 0) {
            copy_len += (size_t)n;
            if (copy_len < sizeof copy_buf - 1)
                continue;
        }
        if (n < 0 && errno == EAGAIN)
            return;
        break; /* the end (or full) */
    }
    close(copy_fd);
    copy_fd = -1;
    copy_buf[copy_len] = 0;
    if (copy_len) {
        clipboard_set(copy_buf);
        offered_sig = text_sig(clipboard_text(), keyboard_focus ? keyboard_focus->client : NULL);
    }
}

static void take_copy(wl_obj *source) {
    if (copy_fd >= 0)
        close(copy_fd), copy_fd = -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
        return;
    wl_send(source, WL_DATA_SOURCE_EV_SEND, "text/plain;charset=utf-8", p[1]);
    close(p[1]);
    copy_fd = p[0];
    copy_len = 0;
}

static void source_gone(wl_obj *o) {
    if (own_source == o)
        own_source = NULL;
}

void wl_clipboard_poll(void) {
    read_copy();
    offer_clipboard();
}

static void data_req(wl_obj *o, int op, union wl_arg *a) {
    if (o->iface == &wl_data_device_manager_iface) {
        if (op == WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE) {
            wl_obj *s = wl_obj_new(o->client, a[0].u, &wl_data_source_iface, o->version, NULL);
            if (s)
                s->destroy = source_gone;
        } else if (op == WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE) {
            wl_obj_new(o->client, a[0].u, &wl_data_device_iface, o->version, NULL);
            offered_sig = 0; /* a new device: offer again */
        }
    } else if ((o->iface == &wl_data_source_iface && op == WL_DATA_SOURCE_DESTROY) ||
               (o->iface == &wl_data_device_iface && op == WL_DATA_DEVICE_RELEASE) ||
               (o->iface == &wl_data_offer_iface && op == WL_DATA_OFFER_DESTROY)) {
        wl_obj_destroy(o);
    } else if (o->iface == &wl_data_device_iface && op == WL_DATA_DEVICE_SET_SELECTION) {
        wl_obj *src = a[0].u ? wl_obj_get(o->client, a[0].u) : NULL;
        if (own_source && own_source != src)
            wl_send(own_source, WL_DATA_SOURCE_EV_CANCELLED);
        own_source = src;
        if (src)
            take_copy(src); /* it becomes the clipboard for everyone */
    } else if (o->iface == &wl_data_offer_iface && op == WL_DATA_OFFER_RECEIVE) {
        int fd = a[1].h;
        const char *t = clipboard_text();
        if (fd >= 0) {
            size_t n = t ? strlen(t) : 0, off = 0;
            int fl = fcntl(fd, F_GETFL);
            if (fl >= 0)
                fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
            while (off < n) {
                ssize_t w = write(fd, t + off, n - off);
                if (w <= 0)
                    break;
                off += (size_t)w;
            }
            close(fd);
        }
    }
}
static void bind_data(wl_client *c, uint32_t id, int v) { wl_obj_new(c, id, &wl_data_device_manager_iface, v, NULL); }

/* ---- input from the shell ---- */

struct each_ctx {
    wl_client *client;
    int what;
    uint32_t a, b, c, d;
    wl_obj *surf;
};

static void to_pointers(wl_obj *p, void *ctx) {
    struct each_ctx *e = ctx;
    if (p->client != e->client)
        return;
    switch (e->what) {
    case 0: wl_send(p, WL_POINTER_EV_ENTER, wl_next_serial(), e->surf, (int)e->a, (int)e->b); break;
    case 1: wl_send(p, WL_POINTER_EV_LEAVE, wl_next_serial(), e->surf); break;
    case 2: wl_send(p, WL_POINTER_EV_MOTION, e->c, (int)e->a, (int)e->b); break;
    case 3: wl_send(p, WL_POINTER_EV_BUTTON, wl_next_serial(), e->c, e->a, e->b); break;
    case 4: wl_send(p, WL_POINTER_EV_AXIS, e->c, WL_POINTER_AXIS_VERTICAL_SCROLL, (int)e->a); break;
    }
    if (p->version >= 5)
        wl_send(p, WL_POINTER_EV_FRAME);
}

static void to_keyboards(wl_obj *k, void *ctx) {
    struct each_ctx *e = ctx;
    if (k->client != e->client)
        return;
    switch (e->what) {
    case 0:
        wl_send(k, WL_KEYBOARD_EV_ENTER, wl_next_serial(), e->surf, (const void *)"", 0u);
        wl_send(k, WL_KEYBOARD_EV_MODIFIERS, wl_next_serial(), e->d, 0, 0, 0);
        break;
    case 1: wl_send(k, WL_KEYBOARD_EV_LEAVE, wl_next_serial(), e->surf); break;
    case 2: wl_send(k, WL_KEYBOARD_EV_KEY, wl_next_serial(), e->c, e->a, e->b); break;
    case 3: wl_send(k, WL_KEYBOARD_EV_MODIFIERS, wl_next_serial(), e->a, 0, 0, 0); break;
    }
}

static int fixed(float v) { return (int)(v * 256.0f); }

void wls_pointer_motion(wl_view *v, float x, float y, uint32_t ms) {
    wl_obj *surf = v && v->surf ? v->surf->obj : NULL;
    if (!surf)
        return;
    x += (float)v->surf->gx, y += (float)v->surf->gy; /* view pixels start at the window geometry */
    if (surf != pointer_focus) {
        wls_pointer_leave();
        pointer_focus = surf;
        struct each_ctx e = {surf->client, 0, (uint32_t)fixed(x), (uint32_t)fixed(y), 0, 0, surf};
        wl_each(&wl_pointer_iface, to_pointers, &e);
    }
    struct each_ctx e = {surf->client, 2, (uint32_t)fixed(x), (uint32_t)fixed(y), ms, 0, surf};
    wl_each(&wl_pointer_iface, to_pointers, &e);
    wl_flush_all();
}

void wls_pointer_leave(void) {
    if (!pointer_focus)
        return;
    struct each_ctx e = {pointer_focus->client, 1, 0, 0, 0, 0, pointer_focus};
    wl_each(&wl_pointer_iface, to_pointers, &e);
    pointer_focus = NULL;
    wl_flush_all();
}

void wls_pointer_button(uint32_t button, bool pressed, uint32_t ms) {
    if (!pointer_focus)
        return;
    struct each_ctx e = {pointer_focus->client, 3, button, pressed ? 1u : 0u, ms, 0, pointer_focus};
    wl_each(&wl_pointer_iface, to_pointers, &e);
    wl_flush_all();
}

void wls_pointer_axis(float dy, uint32_t ms) {
    if (!pointer_focus)
        return;
    struct each_ctx e = {pointer_focus->client, 4, (uint32_t)fixed(dy * 10), 0, ms, 0, pointer_focus};
    wl_each(&wl_pointer_iface, to_pointers, &e);
    wl_flush_all();
}

static uint32_t cur_mods;

void wls_keyboard_focus(wl_view *v) {
    wl_obj *surf = v && v->surf ? v->surf->obj : NULL;
    if (surf == keyboard_focus)
        return;
    if (keyboard_focus) {
        struct each_ctx e = {keyboard_focus->client, 1, 0, 0, 0, 0, keyboard_focus};
        wl_each(&wl_keyboard_iface, to_keyboards, &e);
    }
    keyboard_focus = surf;
    if (surf) {
        struct each_ctx e = {surf->client, 0, 0, 0, 0, cur_mods, surf};
        wl_each(&wl_keyboard_iface, to_keyboards, &e);
    }
    wl_flush_all();
}

void wls_key(uint32_t key, bool pressed, uint32_t mods, uint32_t ms) {
    if (!keyboard_focus)
        return;
    if (mods != cur_mods) {
        cur_mods = mods;
        struct each_ctx e = {keyboard_focus->client, 3, mods, 0, 0, 0, keyboard_focus};
        wl_each(&wl_keyboard_iface, to_keyboards, &e);
    }
    struct each_ctx e = {keyboard_focus->client, 2, key, pressed ? 1u : 0u, ms, 0, keyboard_focus};
    wl_each(&wl_keyboard_iface, to_keyboards, &e);
    wl_flush_all();
}

/* ---- the shell's side ---- */

static void answer_frames(wl_obj *o, void *ctx) {
    struct wl_surf *s = o->data;
    uint32_t ms = *(uint32_t *)ctx;
    while (s && s->ndone) {
        wl_obj *cb = s->done_frames[--s->ndone];
        cb->data = NULL;
        wl_send(cb, WL_CALLBACK_EV_DONE, ms);
        wl_obj_destroy(cb);
    }
}

static void any_waiting(wl_obj *o, void *ctx) {
    struct wl_surf *s = o->data;
    if (s && s->ndone)
        *(bool *)ctx = true;
}

bool wls_frames_waiting(void) {
    bool any = false;
    wl_each(&wl_surface_iface, any_waiting, &any);
    return any;
}

/* Every surface's committed frame callbacks, not only windows': cursors and
 * subsurfaces ask too, and a toolkit waiting on one would stall. */
void wls_frame_done(uint32_t ms) {
    wl_each(&wl_surface_iface, answer_frames, &ms);
    wl_flush_all();
}

void wls_configure(wl_view *v, int w, int h, bool activated) {
    if (v && v->surf) {
        send_configure(v->surf, w, h, activated);
        wl_flush_all();
    }
}

void wls_close(wl_view *v) {
    if (v && v->surf && v->surf->toplevel) {
        wl_send(v->surf->toplevel, XDG_TOPLEVEL_EV_CLOSE);
        wl_flush_all();
    }
}

void wl_client_gone(wl_client *c) {
    (void)c; /* its surfaces are destroyed with its objects, which frees the views */
}

void wl_comp_init(void) {
    wl_register_handler(&wl_compositor_iface, compositor_req);
    wl_register_handler(&wl_subcompositor_iface, subcompositor_req);
    wl_register_handler(&wl_subsurface_iface, subsurface_req);
    wl_register_handler(&wl_surface_iface, surface_req);
    wl_register_handler(&wl_region_iface, region_req);
    wl_register_handler(&wl_shm_iface, shm_req);
    wl_register_handler(&wl_shm_pool_iface, shm_pool_req);
    wl_register_handler(&wl_buffer_iface, buffer_req);
    wl_register_handler(&xdg_wm_base_iface, wm_base_req);
    wl_register_handler(&xdg_surface_iface, xdg_surface_req);
    wl_register_handler(&xdg_toplevel_iface, toplevel_req);
    wl_register_handler(&xdg_popup_iface, popup_req);
    wl_register_handler(&xdg_positioner_iface, positioner_req);
    wl_register_handler(&zxdg_decoration_manager_v1_iface, deco_manager_req);
    wl_register_handler(&zxdg_toplevel_decoration_v1_iface, decoration_req);
    wl_register_handler(&wl_seat_iface, seat_req);
    wl_register_handler(&wl_pointer_iface, input_req);
    wl_register_handler(&wl_keyboard_iface, input_req);
    wl_register_handler(&wl_output_iface, output_req);
    wl_register_handler(&wl_data_device_manager_iface, data_req);
    wl_register_handler(&wl_data_source_iface, data_req);
    wl_register_handler(&wl_data_device_iface, data_req);
    wl_register_handler(&wl_data_offer_iface, data_req);

    wl_global(&wl_compositor_iface, 5, bind_compositor);
    wl_global(&wl_subcompositor_iface, 1, bind_subcompositor);
    wl_global(&wl_shm_iface, 1, bind_shm);
    wl_global(&xdg_wm_base_iface, 5, bind_wm_base);
    wl_global(&zxdg_decoration_manager_v1_iface, 1, bind_deco);
    wl_global(&wl_seat_iface, 7, bind_seat);
    wl_global(&wl_output_iface, 4, bind_output);
    wl_global(&wl_data_device_manager_iface, 3, bind_data);
}

void wls_set_screen(int w, int h) { screen_w = w, screen_h = h; }
