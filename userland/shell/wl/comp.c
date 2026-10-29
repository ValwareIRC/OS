#define _GNU_SOURCE
#include "wlint.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The interfaces apps use to show windows: surfaces with shared-memory
 * buffers, xdg-shell toplevels and popups, the seat (pointer and keyboard)
 * and one output. Each committed frame is copied into the view's pixels
 * (cropped to the window geometry, so client-drawn shadows don't count),
 * and the buffer goes straight back to the app. Decorations are always the
 * shell's: the person's design frames every window. */

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
    free(s);
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

/* Copies the committed buffer into the view (only the window geometry). */
static void take_frame(struct wl_surf *s, struct buffer *b) {
    wl_view *v = s->view;
    if (!v || !b || !b->pool->map)
        return;
    int x0 = 0, y0 = 0, w = b->w, h = b->h;
    if (s->gw > 0 && s->gh > 0) {
        x0 = s->gx, y0 = s->gy, w = s->gw, h = s->gh;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x0 + w > b->w) w = b->w - x0;
        if (y0 + h > b->h) h = b->h - y0;
        if (w <= 0 || h <= 0)
            x0 = y0 = 0, w = b->w, h = b->h;
    }
    if (w != v->w || h != v->h) {
        free(v->px);
        v->px = malloc((size_t)w * (size_t)h * 4);
        v->w = w, v->h = h;
    }
    bool opaque = b->format == WL_SHM_FORMAT_XRGB8888;
    for (int y = 0; y < h; y++) {
        const uint32_t *src = (const uint32_t *)(b->pool->map + b->offset + (size_t)(y + y0) * (size_t)b->stride) + x0;
        uint32_t *dst = v->px + (size_t)y * (size_t)w;
        if (opaque)
            for (int x = 0; x < w; x++)
                dst[x] = src[x] | 0xff000000u;
        else
            memcpy(dst, src, (size_t)w * 4);
    }
    v->version++;
    if (!v->mapped) {
        v->mapped = true;
        wl_host_view_mapped(v);
    }
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
        if (s->attached) {
            if (s->pending_buffer && s->pending_buffer->data) {
                if (s->toplevel && s->view && !s->view->mapped)
                    wl_host_log("wayland: client %p window: first buffer", (void *)o->client);
                take_frame(s, s->pending_buffer->data);
                wl_send(s->pending_buffer, WL_BUFFER_EV_RELEASE);
            } else if (s->view && s->view->mapped) {
                s->view->mapped = false; /* a NULL buffer hides the window */
                wl_host_view_unmapped(s->view);
            }
            s->attached = false;
            s->pending_buffer = NULL;
        }
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

/* Subsurfaces are accepted (toolkits ask for them) but not yet composited. */
static void subsurface_req(wl_obj *o, int op, union wl_arg *a) {
    (void)a;
    if (op == WL_SUBSURFACE_DESTROY)
        wl_obj_destroy(o);
}
static void subcompositor_req(wl_obj *o, int op, union wl_arg *a) {
    if (op == WL_SUBCOMPOSITOR_GET_SUBSURFACE) {
        wl_obj *surf = wl_obj_get(o->client, a[1].u);
        if (surf && surf->data)
            ((struct wl_surf *)surf->data)->role = ROLE_SUB;
        wl_obj_new(o->client, a[0].u, &wl_subsurface_iface, 1, NULL);
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

/* Clipboard and drag and drop: present, so toolkits are content; empty. */
static void data_req(wl_obj *o, int op, union wl_arg *a) {
    if (o->iface == &wl_data_device_manager_iface) {
        if (op == WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE)
            wl_obj_new(o->client, a[0].u, &wl_data_source_iface, o->version, NULL);
        else if (op == WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE)
            wl_obj_new(o->client, a[0].u, &wl_data_device_iface, o->version, NULL);
    } else if ((o->iface == &wl_data_source_iface && op == WL_DATA_SOURCE_DESTROY) ||
               (o->iface == &wl_data_device_iface && op == WL_DATA_DEVICE_RELEASE)) {
        wl_obj_destroy(o);
    } else if (o->iface == &wl_data_source_iface && op == WL_DATA_SOURCE_OFFER) {
        /* nothing to do */
    }
    if (o->iface == &wl_data_source_iface || o->iface == &wl_data_device_iface)
        (void)a;
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
