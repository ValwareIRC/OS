/* A tiny Wayland app for tests/wl-sub-test.sh, drawing the way Firefox and GTK do: its window's surface (red,
 * with a 10-pixel shadow margin outside its window geometry) and two subsurfaces on it. A (blue, synchronized)
 * at (30, 40); B (green, desynchronized) at (70, 60), over A where they meet. Then A gets a yellow frame that
 * the parent doesn't commit, so it must not show (synchronized), and a half-transparent white square C is placed
 * below the parent, so it must not show either. */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_subcompositor *subcomp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_surface *surf, *sa, *sb, *sc;
static struct wl_subsurface *ssa, *ssb, *ssc;
static int configured, drawn;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d, (void)v;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_subcompositor")) subcomp = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d, (void)r, (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void ping(void *d, struct xdg_wm_base *b, uint32_t s) { (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = {ping};

static struct wl_buffer *solid(int w, int h, uint32_t argb, uint32_t format) {
    int size = w * h * 4;
    int fd = memfd_create("buf", 0);
    if (ftruncate(fd, size) < 0)
        exit(1);
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; i++)
        px[i] = argb;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static void show(struct wl_surface *s, struct wl_buffer *b) {
    wl_surface_attach(s, b, 0, 0);
    wl_surface_damage(s, 0, 0, 10000, 10000);
    wl_surface_commit(s);
}

static struct xdg_surface *xs;

static void xs_configure(void *d, struct xdg_surface *x, uint32_t s) {
    (void)d;
    xdg_surface_ack_configure(x, s);
    if (configured++)
        return;
    show(sa, solid(60, 40, 0xff0000ff, WL_SHM_FORMAT_XRGB8888));
    wl_subsurface_set_position(ssa, 30, 40);
    wl_subsurface_set_desync(ssb);
    show(sb, solid(40, 40, 0xff00ff00, WL_SHM_FORMAT_XRGB8888));
    wl_subsurface_set_position(ssb, 70, 60);
    show(sc, solid(30, 30, 0x80808080, WL_SHM_FORMAT_ARGB8888));
    wl_subsurface_set_position(ssc, 20, 20);
    wl_subsurface_place_below(ssc, surf);
    xdg_surface_set_window_geometry(xs, 10, 10, 200, 150);
    show(surf, solid(220, 170, 0xffff0000, WL_SHM_FORMAT_XRGB8888));
    /* a new frame for A that the parent never commits: must not show */
    show(sa, solid(60, 40, 0xffffff00, WL_SHM_FORMAT_XRGB8888));
    drawn = 1;
}
static const struct xdg_surface_listener xs_l = {xs_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d, (void)t, (void)w, (void)h, (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d, (void)t; }
static const struct xdg_toplevel_listener tl_l = {tl_configure, tl_close};

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy)
        return 1;
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !subcomp || !shm || !wm) {
        printf("MISSING globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    surf = wl_compositor_create_surface(comp);
    sa = wl_compositor_create_surface(comp);
    sb = wl_compositor_create_surface(comp);
    sc = wl_compositor_create_surface(comp);
    ssa = wl_subcompositor_get_subsurface(subcomp, sa, surf);
    ssb = wl_subcompositor_get_subsurface(subcomp, sb, surf);
    ssc = wl_subcompositor_get_subsurface(subcomp, sc, surf);
    xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_l, NULL);
    xdg_toplevel_set_title(tl, "subsurface test");
    wl_surface_commit(surf);
    time_t end = time(NULL) + 8; /* (past the compositor saving what it shows) */
    while (time(NULL) < end) {
        wl_display_flush(dpy);
        struct pollfd pf = {wl_display_get_fd(dpy), POLLIN, 0};
        if (poll(&pf, 1, 200) > 0 && wl_display_dispatch(dpy) < 0) {
            printf("DISCONNECTED (a protocol error?)\n");
            return 1;
        }
        wl_display_dispatch_pending(dpy);
    }
    printf("DONE drawn=%d\n", drawn);
    return 0;
}
