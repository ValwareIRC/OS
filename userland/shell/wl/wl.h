#pragma once
#include <stdbool.h>
#include <stdint.h>

/* The OS's Wayland compositor: Linux desktop apps (GTK, SDL, Qt, Electron
 * on Wayland) draw into shared memory and hand their frames to the shell,
 * which shows them in windows framed by the person's own design.
 *
 * The shell calls wls_start once, wls_dispatch every frame, and reads the
 * views (one per app window). Input goes back through wls_pointer_* and
 * wls_key. Hooks in wl_host_* tell the shell when views come and go. */

typedef struct wl_view {
    int id;
    char title[128], app_id[64];
    int w, h;          /* size of the last frame */
    uint32_t *px;      /* its pixels (ARGB, w*h), owned by the compositor */
    bool mapped;       /* has shown a frame */
    bool want_close;   /* the app asked to be closed (or crashed) */
    uint64_t version;  /* bumps on every new frame */
    int min_w, min_h;
    void *host;        /* the shell's window for it */
    bool popup;        /* a menu or tooltip: drawn over its parent, at min_w,min_h */
    struct wl_view *parent;
    struct wl_surf *surf;
    int max_w, max_h;    /* 0: no limit */
    unsigned wm_state;   /* WLS_STATE_*: told to the app with every configure */
    bool client_frame;   /* the app draws its own title bar and frame (xdg-decoration client side): the shell draws none */
} wl_view;

/* Window states the shell's window manager sets on a view. */
#define WLS_STATE_MAXIMIZED 1u
#define WLS_STATE_RESIZING 2u
#define WLS_STATE_TILED_LEFT 4u
#define WLS_STATE_TILED_RIGHT 8u
#define WLS_STATE_TILED_TOP 16u
#define WLS_STATE_TILED_BOTTOM 32u

/* What an app can ask of its window (xdg_toplevel requests); the shell's
 * frame does them. edges: XDG resize edges (top 1, bottom 2, left 4, right 8). */
enum { WLS_REQ_MOVE, WLS_REQ_RESIZE, WLS_REQ_MAXIMIZE, WLS_REQ_UNMAXIMIZE, WLS_REQ_MINIMIZE };

bool wls_start(const char *runtime_dir); /* creates runtime_dir/wayland-0 */
/* Accepts clients, reads their requests, applies them. Never blocks. */
void wls_dispatch(void);
/* File descriptors to wait on (listen socket and clients); returns count. */
int wls_fds(int *fds, int max);
/* Tells apps a frame was shown (they draw the next one then). */
void wls_frame_done(uint32_t ms);

int wls_view_count(void);
wl_view *wls_view(int i);
/* Ask the app to take this size (0, 0: its own choice). */
void wls_configure(wl_view *v, int w, int h, bool activated);
void wls_close(wl_view *v);

/* Input, in view-local pixels. button: Linux BTN_* code. */
void wls_pointer_motion(wl_view *v, float x, float y, uint32_t ms);
void wls_pointer_leave(void);
void wls_pointer_button(uint32_t button, bool pressed, uint32_t ms);
void wls_pointer_axis(float dy, uint32_t ms);
void wls_keyboard_focus(wl_view *v);
/* key: Linux KEY_* code (evdev). mods: shift 1, ctrl 4, alt 8, logo 64. */
void wls_key(uint32_t key, bool pressed, uint32_t mods, uint32_t ms);

/* Provided by the shell. */
void wl_host_view_mapped(wl_view *v);
void wl_host_view_unmapped(wl_view *v);
void wl_host_view_request(wl_view *v, int req, uint32_t edges);
void wl_host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void wls_set_screen(int w, int h);
