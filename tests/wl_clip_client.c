/* A tiny Wayland app for tests/wl-clip-test.sh: it shows a window, pastes
 * what the compositor offers once it has the keyboard ("PASTE: ..."), then
 * copies text of its own, which the compositor should read and take. */
#define _GNU_SOURCE
#include <fcntl.h>
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
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct wl_surface *surf;
static struct wl_keyboard *kbd;
static struct wl_data_device *dev;
static struct wl_data_source *src;
static uint32_t enter_serial;
static int pasted, copied;
static struct wl_display *dpy;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d;
    if (!strcmp(iface, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(iface, "wl_seat")) seat = wl_registry_bind(r, name, &wl_seat_interface, v < 5 ? v : 5);
    else if (!strcmp(iface, "wl_data_device_manager")) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d, (void)r, (void)n; }
static const struct wl_registry_listener reg_l = {reg_global, reg_remove};

static void ping(void *d, struct xdg_wm_base *b, uint32_t s) { (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = {ping};

static void draw(void) {
    int w = 64, h = 64, size = w * h * 4;
    int fd = memfd_create("buf", 0);
    if (ftruncate(fd, size) < 0)
        exit(1);
    uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; i++)
        px[i] = 0xff3366cc;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, w, h);
    wl_surface_commit(surf);
}
static void xs_configure(void *d, struct xdg_surface *x, uint32_t s) {
    (void)d;
    xdg_surface_ack_configure(x, s);
    draw();
}
static const struct xdg_surface_listener xs_l = {xs_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *s) { (void)d, (void)t, (void)w, (void)h, (void)s; }
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d, (void)t; }
static const struct xdg_toplevel_listener tl_l = {tl_configure, tl_close};

/* our own copy: handed over when asked */
static void src_target(void *d, struct wl_data_source *s, const char *m) { (void)d, (void)s, (void)m; }
static void src_send(void *d, struct wl_data_source *s, const char *mime, int fd) {
    (void)d, (void)s;
    const char *t = "copied in an app";
    if (write(fd, t, strlen(t)) < 0)
        perror("write");
    close(fd);
    printf("SENT our copy as %s\n", mime);
    fflush(stdout);
    copied = 1;
}
static void src_cancelled(void *d, struct wl_data_source *s) { (void)d; wl_data_source_destroy(s); }
static const struct wl_data_source_listener src_l = {src_target, src_send, src_cancelled};

static void copy_ours(void) {
    src = wl_data_device_manager_create_data_source(ddm);
    wl_data_source_add_listener(src, &src_l, NULL);
    wl_data_source_offer(src, "text/plain;charset=utf-8");
    wl_data_device_set_selection(dev, src, enter_serial);
    wl_display_flush(dpy);
}

static void offer_offer(void *d, struct wl_data_offer *o, const char *mime) { (void)d, (void)o; printf("OFFERED %s\n", mime); }
static const struct wl_data_offer_listener offer_l = {offer_offer};

static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)d, (void)dd;
    wl_data_offer_add_listener(o, &offer_l, NULL);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y,
                     struct wl_data_offer *o) { (void)d, (void)dd, (void)s, (void)sf, (void)x, (void)y, (void)o; }
static void dd_leave(void *d, struct wl_data_device *dd) { (void)d, (void)dd; }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d, (void)dd, (void)t, (void)x, (void)y; }
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d, (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o) {
    (void)d, (void)dd;
    if (!o || pasted)
        return;
    int p[2];
    if (pipe(p) < 0)
        return;
    wl_data_offer_receive(o, "text/plain;charset=utf-8", p[1]);
    close(p[1]);
    wl_display_flush(dpy);
    char buf[4096];
    size_t len = 0;
    for (;;) {
        struct pollfd pf = {p[0], POLLIN, 0};
        if (poll(&pf, 1, 2000) <= 0)
            break;
        ssize_t n = read(p[0], buf + len, sizeof buf - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    close(p[0]);
    buf[len] = 0;
    printf("PASTE: %s\n", buf);
    fflush(stdout);
    pasted = 1;
    copy_ours();
}
static const struct wl_data_device_listener dd_l = {dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection};

static void k_map(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t s) { (void)d, (void)k, (void)f, (void)s; close(fd); }
static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *a) {
    (void)d, (void)k, (void)sf, (void)a;
    enter_serial = s;
    printf("KEYBOARD\n");
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf) { (void)d, (void)k, (void)s, (void)sf; }
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st) { (void)d, (void)k, (void)s, (void)t, (void)key, (void)st; }
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { (void)d, (void)k, (void)s, (void)a, (void)b, (void)c, (void)g; }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t de) { (void)d, (void)k, (void)r, (void)de; }
static const struct wl_keyboard_listener k_l = {k_map, k_enter, k_leave, k_key, k_mods, k_repeat};

int main(void) {
    dpy = wl_display_connect(NULL);
    if (!dpy)
        return 1;
    struct wl_registry *r = wl_display_get_registry(dpy);
    wl_registry_add_listener(r, &reg_l, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shm || !wm || !seat || !ddm) {
        printf("MISSING globals\n");
        return 1;
    }
    xdg_wm_base_add_listener(wm, &wm_l, NULL);
    kbd = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(kbd, &k_l, NULL);
    dev = wl_data_device_manager_get_data_device(ddm, seat);
    wl_data_device_add_listener(dev, &dd_l, NULL);
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
    xdg_surface_add_listener(xs, &xs_l, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &tl_l, NULL);
    xdg_toplevel_set_title(tl, "clip test");
    wl_surface_commit(surf);
    time_t end = time(NULL) + 8;
    while (time(NULL) < end && !(pasted && copied)) {
        wl_display_flush(dpy);
        struct pollfd pf = {wl_display_get_fd(dpy), POLLIN, 0};
        if (poll(&pf, 1, 200) > 0 && wl_display_dispatch(dpy) < 0)
            break;
        wl_display_dispatch_pending(dpy);
    }
    printf("DONE pasted=%d copied=%d\n", pasted, copied);
    return 0;
}
