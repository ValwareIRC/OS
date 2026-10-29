#define _GNU_SOURCE
#include "wm.h"
#include "launcher.h"
#include "app.h"
#include "fm/fm.h"
#include "wl/wl.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The window manager. Windows here are desktop.c's A.win[] slots: the
 * built-in apps first, then one per Linux app window. Everything a person
 * does to a window as a whole (move, resize, snap, minimize, switch to it)
 * happens here, the same way for both kinds. */

#define D (A.d)

/* Linux key codes (the raw events every frame carries) */
#define KEY_ESC 1
#define KEY_TAB 15
#define KEY_LEFTALT 56
#define KEY_F4 62
#define KEY_RIGHTALT 100
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126

#define GAP 12        /* space kept around snapped and maximized windows */
#define BORDER_OUT 6  /* the grab band for resizing reaches this far outside a window */
#define BORDER_IN 4   /* and this far in */
#define CORNER 18     /* along an edge, this close to a corner resizes both ways */
#define MARGIN 110    /* room around a window for its shadow when it is redrawn elsewhere */

enum { E_L = 1, E_R = 2, E_T = 4, E_B = 8 };
enum { SNAP_NONE, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT, SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR };
static const char *const snap_names[] = {"none", "max", "left", "right", "top-left", "top-right", "bottom-left",
                                         "bottom-right"};
enum { ANIM_NONE, ANIM_OPEN, ANIM_RECT, ANIM_RESTORE };
enum { MODE_NONE, MODE_MOVE, MODE_RESIZE };

static struct wmw {
    int snap;
    rect floating;          /* its size and place before it was snapped */
    int anim;               /* a live window drawn on its way somewhere */
    uint64_t anim_at;
    int anim_ms;
    rect from;
    bool pending_close, pending_min; /* done once this frame's picture of it is kept */
} ww[WIN_MAX];

/* The window being moved or resized. */
static struct {
    int mode, win, edges;
    int sx, sy;     /* pointer when pressed */
    rect start;     /* the window then */
    int dx, dy;     /* where in the window it was held */
    bool moved;
    bool was_free;  /* not snapped when picked up: `start` is where it goes back to */
    int target;     /* where it would snap if let go now */
    rect preview;   /* the translucent outline of that, easing towards it */
    float preview_a;
    uint64_t last_t;
} it = {.win = -1};

static struct {
    int w, x, y;
    uint64_t t;
} last_press = {-1, 0, 0, 0};
static int hover_win = -1, hover_edges;

/* ---- small helpers ---- */

static uint64_t usec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static float clamp01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static float ease_out(float t) {
    t = clamp01(t);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
static float ease_in_out(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}

static rect lerp_rect(rect a, rect b, float t) {
    return (rect){(int)lroundf(a.x + (b.x - a.x) * t), (int)lroundf(a.y + (b.y - a.y) * t),
                  (int)lroundf(a.w + (b.w - a.w) * t), (int)lroundf(a.h + (b.h - a.h) * t)};
}

static rect scale_about_centre(rect r, float s) {
    int w = (int)lroundf(r.w * s), h = (int)lroundf(r.h * s);
    return (rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
}

/* A window-shaped rect the size of `item`, centred on it: what a window
 * shrinks into when it flies into its taskbar item. */
static rect shrink_into(rect item, rect win) {
    int w = item.w > 8 ? item.w : 8, h = win.w > 0 ? w * win.h / win.w : w;
    return (rect){item.x + (item.w - w) / 2, item.y + (item.h - h) / 2, w, h > 4 ? h : 4};
}

static bool reduced(void) { return A.prof.reduce_motion || D.motion <= 0.01f; }

static rect work(void) { return A.ui && A.ui->work.w > 200 ? A.ui->work : (rect){0, 0, A.W, A.H}; }

static bool overlaps(rect a, rect b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

/* The front window: the one keys go to (desktop.c's focused_window). */
static int focused(void) {
    int best = -1;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open && !A.win[w].minimized && (best < 0 || A.win[w].z > A.win[best].z))
            best = w;
    return best;
}

/* Its place among the open windows, as <windowlist> numbers them. */
static int open_index(int w) {
    int i = 0;
    for (int k = 0; k < w; k++)
        i += A.win[k].open;
    return A.win[w].open ? i : -1;
}

/* The frame's own style (radius, shadow), from the design's window template. */
static const fm_style *frame_style(void) {
    if (!A.ui || !A.ui->window_tpl)
        return NULL;
    for (fm_node *k = A.ui->window_tpl->first; k; k = k->next)
        if (k->kind == FM_ELEM && k->laid_out)
            return &k->st;
    return NULL;
}

static void frame_radii(float rad[4], rect r) {
    const fm_style *fs = frame_style();
    float m = (r.w < r.h ? r.w : r.h) / 2.0f;
    for (int i = 0; i < 4; i++)
        rad[i] = fminf(fs ? fs->radius[i] : D.radius, m);
}

static void *grow(void **buf, size_t *cap, size_t bytes) {
    if (bytes > *cap) {
        free(*buf);
        *buf = malloc(bytes);
        *cap = *buf ? bytes : 0;
    }
    return *buf;
}

/* ---- where a window goes when minimized: its item in the design ---- */

static bool shown(fm_node *n) {
    for (; n; n = n->parent) {
        if (n->kind != FM_ELEM)
            continue;
        if (n->st.hidden || !n->laid_out || (fm_layer_of(n) == 2 && !n->open))
            return false;
    }
    return true;
}

static fm_node *find_item(fm_node *n, int idx, int app) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (n->generated && !strcmp(n->tag, "item") && (idx >= 0 ? n->win == idx : n->win < 0 && n->app == app) &&
            n->vis.w > 0 && shown(n))
            return n;
        fm_node *f = find_item(n->first, idx, app);
        if (f)
            return f;
    }
    return NULL;
}

/* Its windowlist item, else its app's icon (a dock), else off the bottom. */
static rect item_rect(int w) {
    int idx = open_index(w);
    fm_node *n = NULL;
    if (A.ui && A.ui->desktop && idx >= 0) {
        n = find_item(A.ui->desktop->first, idx, -1);
        if (!n)
            n = find_item(A.ui->desktop->first, -1, fm_host_window_app(idx));
    }
    return n ? n->vis : (rect){A.W / 2 - 30, A.H + 10, 60, 40};
}

/* ---- drawing a window somewhere other than where it is ---- */

/* Coverage of a point inside a w x h rounded rectangle (per-corner radii). */
static float corner_cov(float x, float y, int w, int h, const float rad[4]) {
    bool left = x < w * 0.5f, top = y < h * 0.5f;
    float r = top ? (left ? rad[0] : rad[1]) : (left ? rad[3] : rad[2]);
    if (r <= 0)
        return 1;
    float cx = left ? r : w - r, cy = top ? r : h - r;
    if ((left ? x >= cx : x <= cx) || (top ? y >= cy : y <= cy))
        return 1;
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    return clamp01(r + 0.5f - d);
}

/* Draws a picture of a window (premultiplied: transparent where it was off
 * screen) stretched into `dst`, with its rounded corners, at `alpha`. */
static void composite(const uint32_t *img, int iw, int ih, const float rad[4], rect dst, float alpha) {
    if (dst.w < 1 || dst.h < 1 || alpha <= 0.004f || iw < 2 || ih < 2)
        return;
    canvas *c = A.frame;
    int x0 = dst.x < 0 ? 0 : dst.x, y0 = dst.y < 0 ? 0 : dst.y;
    int x1 = dst.x + dst.w > c->w ? c->w : dst.x + dst.w, y1 = dst.y + dst.h > c->h ? c->h : dst.y + dst.h;
    float sx = (float)iw / dst.w, sy = (float)ih / dst.h;
    uint32_t ga = (uint32_t)(clamp01(alpha) * 256);
    for (int y = y0; y < y1; y++) {
        float v = (y + 0.5f - dst.y) * sy - 0.5f;
        int vi = (int)floorf(v);
        uint32_t ty = (uint32_t)((v - vi) * 256);
        const uint32_t *r0 = vi >= 0 && vi < ih ? &img[(size_t)vi * iw] : NULL;
        const uint32_t *r1 = vi + 1 >= 0 && vi + 1 < ih ? &img[(size_t)(vi + 1) * iw] : NULL;
        float vs = v + 0.5f;
        bool corner_row = vs < rad[0] + 1 || vs < rad[1] + 1 || vs > ih - rad[2] - 1 || vs > ih - rad[3] - 1;
        uint32_t *row = &c->px[(size_t)y * c->w];
        for (int x = x0; x < x1; x++) {
            float u = (x + 0.5f - dst.x) * sx - 0.5f;
            int ui = (int)floorf(u);
            uint32_t tx = (uint32_t)((u - ui) * 256);
            bool in0 = ui >= 0 && ui < iw, in1 = ui + 1 >= 0 && ui + 1 < iw;
            uint32_t p00 = r0 && in0 ? r0[ui] : 0, p10 = r0 && in1 ? r0[ui + 1] : 0;
            uint32_t p01 = r1 && in0 ? r1[ui] : 0, p11 = r1 && in1 ? r1[ui + 1] : 0;
            uint32_t k = ga;
            if (corner_row) {
                float cv = corner_cov(u + 0.5f, vs, iw, ih, rad);
                if (cv <= 0)
                    continue;
                k = (uint32_t)(k * cv);
            }
            uint32_t out[4];
            for (int s = 0; s < 4; s++) {
                int sh = s * 8;
                uint32_t top = ((p00 >> sh) & 0xff) * (256 - tx) + ((p10 >> sh) & 0xff) * tx;
                uint32_t bot = ((p01 >> sh) & 0xff) * (256 - tx) + ((p11 >> sh) & 0xff) * tx;
                out[s] = ((top * (256 - ty) + bot * ty) >> 16) * k >> 8;
            }
            uint32_t a = out[3];
            if (!a)
                continue;
            uint32_t d = row[x], na = 255 - (a > 255 ? 255 : a);
            uint32_t R = out[2] + C_R(d) * na / 255, G = out[1] + C_G(d) * na / 255, B = out[0] + C_B(d) * na / 255;
            row[x] = 0xff000000u | (R > 255 ? 255 : R) << 16 | (G > 255 ? 255 : G) << 8 | (B > 255 ? 255 : B);
        }
    }
}

/* The frame's shadow under a window drawn somewhere else. */
static void ghost_shadow(rect dst, rect orig, float alpha) {
    const fm_style *fs = frame_style();
    if (!fs || fs->shadow_blur <= 0 || !C_A(fs->shadow) || alpha <= 0.02f)
        return;
    float s = orig.w > 0 ? (float)dst.w / orig.w : 1;
    float rad[4];
    frame_radii(rad, orig);
    shadow_round(A.frame, (rect){dst.x + (int)(fs->shadow_x * s), dst.y + (int)(fs->shadow_y * s), dst.w, dst.h},
                 rad[0] * s, fmaxf(4, fs->shadow_blur * s), color_alpha(fs->shadow, alpha * alpha));
}

/* Copies a region of the frame (premultiplied, off-screen parts transparent). */
static void grab(rect r, uint32_t *out) {
    for (int j = 0; j < r.h; j++) {
        int y = r.y + j;
        uint32_t *o = &out[(size_t)j * r.w];
        if (y < 0 || y >= A.H) {
            memset(o, 0, (size_t)r.w * 4);
            continue;
        }
        for (int i = 0; i < r.w; i++) {
            int x = r.x + i;
            o[i] = x >= 0 && x < A.W ? A.frame->px[(size_t)y * A.W + x] | 0xff000000u : 0;
        }
    }
}

static rect on_screen(rect r) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > A.W ? A.W : r.x + r.w, y1 = r.y + r.h > A.H ? A.H : r.y + r.h;
    return (rect){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
}

static void save_region(rect r, uint32_t *buf) {
    for (int j = 0; j < r.h; j++)
        memcpy(&buf[(size_t)j * r.w], &A.frame->px[(size_t)(r.y + j) * A.W + r.x], (size_t)r.w * 4);
}

static void restore_region(rect r, const uint32_t *buf) {
    for (int j = 0; j < r.h; j++)
        memcpy(&A.frame->px[(size_t)(r.y + j) * A.W + r.x], &buf[(size_t)j * r.w], (size_t)r.w * 4);
}

/* ---- ghosts: windows that are gone but still fading or flying ---- */

#define NGHOST 6
enum { GHOST_CLOSE, GHOST_MIN };
static struct ghost {
    bool used, waiting; /* waiting: an app was asked to close and hasn't yet */
    int w, kind;
    uint32_t *px;
    int pw, ph;
    rect from, to;
    uint64_t at;
    int ms;
} ghosts[NGHOST];

static struct ghost *new_ghost(int w, const uint32_t *img, rect from) {
    struct ghost *g = NULL;
    for (int i = 0; i < NGHOST && !g; i++)
        if (!ghosts[i].used || ghosts[i].w == w)
            g = &ghosts[i];
    if (!g) { /* all busy: the oldest gives way */
        g = &ghosts[0];
        for (int i = 1; i < NGHOST; i++)
            if (ghosts[i].at < g->at)
                g = &ghosts[i];
    }
    free(g->px);
    *g = (struct ghost){.used = true, .w = w, .from = from, .at = A.now, .pw = from.w, .ph = from.h};
    g->px = malloc((size_t)from.w * from.h * 4);
    if (!g->px) {
        g->used = false;
        return NULL;
    }
    memcpy(g->px, img, (size_t)from.w * from.h * 4);
    return g;
}

void wm_draw_ghosts(void) {
    for (int i = 0; i < NGHOST; i++) {
        struct ghost *g = &ghosts[i];
        if (!g->used)
            continue;
        if (g->waiting) {
            if (A.now - g->at > 2500) { /* the app didn't close after all (or asked them first) */
                free(g->px);
                g->px = NULL;
                g->used = false;
            }
            continue;
        }
        float t = (float)(A.now - g->at) / (float)g->ms;
        if (t >= 1) {
            free(g->px);
            g->px = NULL;
            g->used = false;
            continue;
        }
        rect dst;
        float alpha;
        if (g->kind == GHOST_MIN && !reduced()) {
            float e = ease_in_out(t);
            dst = lerp_rect(g->from, shrink_into(g->to, g->from), e);
            alpha = 1 - 0.8f * e * e;
        } else {
            float e = ease_out(t);
            dst = reduced() ? g->from : scale_about_centre(g->from, 1 - 0.05f * e);
            alpha = 1 - e;
        }
        float rad[4];
        frame_radii(rad, g->from);
        ghost_shadow(dst, g->from, alpha);
        composite(g->px, g->pw, g->ph, rad, dst, alpha);
    }
}

/* ---- live animations of open windows ---- */

static void start_anim(int w, int kind, rect from, int ms) {
    ww[w].anim = kind;
    ww[w].anim_at = A.now;
    ww[w].anim_ms = ms;
    ww[w].from = from;
}

/* Where window w appears this frame and how opaque; false when it is simply
 * where it is. */
static bool live_anim(int w, rect *dst, float *alpha) {
    struct wmw *m = &ww[w];
    if (!m->anim)
        return false;
    float t = (float)(A.now - m->anim_at) / (float)m->anim_ms;
    if (t >= 1) {
        m->anim = ANIM_NONE;
        return false;
    }
    rect r = A.win[w].r;
    float e = ease_out(t);
    switch (m->anim) {
    case ANIM_OPEN:
        *dst = reduced() ? r : scale_about_centre(r, 0.92f + 0.08f * e);
        *alpha = reduced() ? t : e;
        break;
    case ANIM_RECT:
        *dst = lerp_rect(m->from, r, e);
        *alpha = 1;
        break;
    default: /* ANIM_RESTORE: out of its taskbar item */
        if (reduced()) {
            *dst = r;
            *alpha = t;
        } else {
            e = ease_in_out(t);
            *dst = lerp_rect(shrink_into(m->from, r), r, e);
            *alpha = 0.2f + 0.8f * sqrtf(e);
        }
        break;
    }
    return true;
}

static void move_to(int w, rect to) {
    struct window *win = &A.win[w];
    if (!reduced() && memcmp(&win->r, &to, sizeof to) && win->r.w > 0)
        start_anim(w, ANIM_RECT, win->r, 230);
    win->r = to;
}

/* ---- snapping ---- */

static rect snap_rect(int s) {
    rect k = work();
    int hw = (k.w - 3 * GAP) / 2, hh = (k.h - 3 * GAP) / 2, fh = k.h - 2 * GAP;
    int l = k.x + GAP, r = k.x + k.w - GAP - hw, t = k.y + GAP, b = k.y + k.h - GAP - hh;
    switch (s) {
    case SNAP_LEFT: return (rect){l, t, hw, fh};
    case SNAP_RIGHT: return (rect){r, t, hw, fh};
    case SNAP_TL: return (rect){l, t, hw, hh};
    case SNAP_TR: return (rect){r, t, hw, hh};
    case SNAP_BL: return (rect){l, b, hw, hh};
    case SNAP_BR: return (rect){r, b, hw, hh};
    default: return rect_inset(k, GAP);
    }
}

/* Tells an app window its state (maximized, tiled, being resized). */
static void tell_app(int w) {
    wl_view *v = w >= WIN_COUNT ? A.win[w].view : NULL;
    if (!v)
        return;
    unsigned st = 0;
    switch (ww[w].snap) {
    case SNAP_MAX: st = WLS_STATE_MAXIMIZED; break;
    case SNAP_LEFT: st = WLS_STATE_TILED_LEFT | WLS_STATE_TILED_TOP | WLS_STATE_TILED_BOTTOM; break;
    case SNAP_RIGHT: st = WLS_STATE_TILED_RIGHT | WLS_STATE_TILED_TOP | WLS_STATE_TILED_BOTTOM; break;
    case SNAP_TL: st = WLS_STATE_TILED_LEFT | WLS_STATE_TILED_TOP; break;
    case SNAP_TR: st = WLS_STATE_TILED_RIGHT | WLS_STATE_TILED_TOP; break;
    case SNAP_BL: st = WLS_STATE_TILED_LEFT | WLS_STATE_TILED_BOTTOM; break;
    case SNAP_BR: st = WLS_STATE_TILED_RIGHT | WLS_STATE_TILED_BOTTOM; break;
    }
    if (it.mode == MODE_RESIZE && it.win == w)
        st |= WLS_STATE_RESIZING;
    if (st != v->wm_state) {
        v->wm_state = st;
        /* the size follows from the frame (desktop.c asks for it); the state is news now */
        if (A.win[w].fitted && A.win[w].asked_w > 0)
            wls_configure(v, A.win[w].asked_w, A.win[w].asked_h, w == focused());
    }
}

static void set_snap(int w, int s) {
    struct wmw *m = &ww[w];
    struct window *win = &A.win[w];
    if (s != SNAP_NONE && m->snap == SNAP_NONE)
        m->floating = win->r;
    rect to = s == SNAP_NONE ? (m->floating.w > 0 ? m->floating : win->r) : snap_rect(s);
    m->snap = s;
    win->maximized = s == SNAP_MAX;
    win->restore = m->floating;
    move_to(w, to);
    tell_app(w);
    LOG("wm snap win-%d %s\n", w, snap_names[s]);
}

/* Where a window held at the pointer would snap: the screen's (or the free
 * area's) sides make halves, its top maximizes, near corners make quarters. */
static int snap_target(int mx, int my) {
    rect k = work();
    const int E = 8, C = 80;
    bool l = mx <= k.x + E || mx <= E, r = mx >= k.x + k.w - 1 - E || mx >= A.W - 1 - E;
    bool t = my <= k.y + E || my <= E;
    if (l || r) {
        if (my < k.y + C)
            return l ? SNAP_TL : SNAP_TR;
        if (my > k.y + k.h - C)
            return l ? SNAP_BL : SNAP_BR;
        return l ? SNAP_LEFT : SNAP_RIGHT;
    }
    if (t)
        return mx < k.x + C ? SNAP_TL : mx > k.x + k.w - C ? SNAP_TR : SNAP_MAX;
    return SNAP_NONE;
}

/* ---- the picture below a window being moved ---- */

static struct {
    canvas *c;
    bool valid, using;
    int win;
    uint64_t at;
    uint64_t ver[WIN_MAX];
} scene;

static bool interacting(void) { return A.screen == SCR_DESKTOP && it.mode != MODE_NONE; }

static void capture_scene(void) {
    if (!scene.c || scene.c->w != A.W || scene.c->h != A.H) {
        canvas_free(scene.c);
        scene.c = canvas_new(A.W, A.H);
        if (!scene.c)
            return;
    }
    memcpy(scene.c->px, A.frame->px, (size_t)A.W * A.H * 4);
    for (int w = WIN_COUNT; w < WIN_MAX; w++)
        scene.ver[w] = A.win[w].view ? A.win[w].view->version : 0;
    scene.valid = true;
    scene.win = it.win;
    scene.at = A.now;
}

/* App windows below keep playing: their new frames are put straight into
 * the kept picture when nothing else there covers them. Otherwise the
 * picture is redrawn, now and then. */
static void refresh_apps_below(void) {
    int az = A.win[scene.win].z;
    bool stale = false;
    for (int w = WIN_COUNT; w < WIN_MAX; w++) {
        struct window *win = &A.win[w];
        wl_view *v = win->view;
        if (w == scene.win || !win->open || win->minimized || !v || !v->px || win->z > az || v->version == scene.ver[w])
            continue;
        bool covered = false;
        for (int u = 0; u < WIN_MAX && !covered; u++)
            covered = u != w && u != scene.win && A.win[u].open && !A.win[u].minimized && A.win[u].z > win->z &&
                      A.win[u].z < az && overlaps(A.win[u].r, win->body);
        for (int i = 0; i < wls_view_count() && !covered; i++) {
            wl_view *p = wls_view(i);
            covered = p->popup && p->parent == v && p->mapped;
        }
        if (covered) {
            stale = true;
            continue;
        }
        canvas_clip(scene.c, win->body);
        fill_rect(scene.c, win->body, D.surface);
        canvas src = {v->w, v->h, v->px, 0, 0, v->w, v->h};
        canvas_blit(scene.c, &src, win->body.x, win->body.y);
        canvas_unclip(scene.c);
        scene.ver[w] = v->version;
    }
    if (stale && A.now - scene.at > 500)
        scene.valid = false; /* drawn whole again next frame */
}

bool wm_scene_cached(void) {
    scene.using = false;
    if (!interacting() || !scene.valid || scene.win != it.win || !scene.c) {
        scene.valid = scene.valid && interacting() && scene.win == it.win;
        return false;
    }
    refresh_apps_below();
    canvas_unclip(A.frame);
    memcpy(A.frame->px, scene.c->px, (size_t)A.W * A.H * 4);
    scene.using = true;
    return true;
}

/* ---- moving and resizing ---- */

static void min_size(int w, int *mw, int *mh) {
    struct window *win = &A.win[w];
    int dw = win->body.w > 0 ? win->r.w - win->body.w : 24, dh = win->body.h > 0 ? win->r.h - win->body.h : 64;
    *mw = 320, *mh = 220;
    if (w == WIN_COMPANION)
        *mw = 300, *mh = 320;
    if (w >= WIN_COUNT && win->view) {
        *mw = 200 > win->view->min_w + dw ? 200 : win->view->min_w + dw;
        *mh = 140 > win->view->min_h + dh ? 140 : win->view->min_h + dh;
    }
}

static void end_interaction(void) {
    int w = it.win;
    int mode = it.mode;
    bool snap = mode == MODE_MOVE && it.moved && it.target != SNAP_NONE && w >= 0 && A.win[w].open;
    it.mode = MODE_NONE;
    it.win = -1;
    if (A.drag_win == w)
        A.drag_win = -1;
    scene.valid = false;
    if (w < 0 || w >= WIN_MAX)
        return;
    if (snap) {
        /* Restoring later puts it back where it was picked up, not where it
         * was dropped (the edge of the screen). */
        rect back = it.was_free ? it.start : A.win[w].r;
        set_snap(w, it.target);
        ww[w].floating = A.win[w].restore = back;
    }
    tell_app(w); /* no longer resizing */
    if (mode == MODE_RESIZE)
        LOG("wm resized win-%d to %dx%d\n", w, A.win[w].r.w, A.win[w].r.h);
}

static void start_resize(int w, int edges) {
    struct window *win = &A.win[w];
    if (ww[w].snap != SNAP_NONE) { /* resizing a snapped window makes it free again, at that size */
        ww[w].snap = SNAP_NONE;
        win->maximized = false;
    }
    it = (typeof(it)){.mode = MODE_RESIZE, .win = w, .edges = edges, .sx = A.in.mx, .sy = A.in.my, .start = win->r,
                      .last_t = A.now};
    A.drag_win = w;
    win->z = ++A.ztop;
    tell_app(w);
}

static void update_resize(void) {
    int w = it.win, mw, mh;
    rect s = it.start, r = s;
    int dx = A.in.mx - it.sx, dy = A.in.my - it.sy;
    min_size(w, &mw, &mh);
    if (it.edges & E_L) {
        r.x = s.x + dx, r.w = s.w - dx;
        if (r.w < mw)
            r.w = mw, r.x = s.x + s.w - mw;
    }
    if (it.edges & E_R)
        r.w = s.w + dx < mw ? mw : s.w + dx;
    if (it.edges & E_T) {
        r.y = s.y + dy, r.h = s.h - dy;
        if (r.y < 0)
            r.h += r.y, r.y = 0;
        if (r.h < mh)
            r.h = mh, r.y = s.y + s.h - mh;
    }
    if (it.edges & E_B)
        r.h = s.h + dy < mh ? mh : s.h + dy;
    wl_view *v = w >= WIN_COUNT ? A.win[w].view : NULL;
    if (v && v->max_w > 0 && r.w > v->max_w + (s.w - A.win[w].body.w)) {
        int maxw = v->max_w + (s.w - A.win[w].body.w);
        if (it.edges & E_L)
            r.x += r.w - maxw;
        r.w = maxw;
    }
    if (v && v->max_h > 0 && r.h > v->max_h + (s.h - A.win[w].body.h)) {
        int maxh = v->max_h + (s.h - A.win[w].body.h);
        if (it.edges & E_T)
            r.y += r.h - maxh;
        r.h = maxh;
    }
    A.win[w].r = r;
}

static void update_move(void) {
    int w = it.win;
    struct window *win = &A.win[w];
    int mx = A.in.mx, my = A.in.my;
    float dt = (float)(A.now - it.last_t);
    it.last_t = A.now;
    if (!it.moved) {
        if (abs(mx - it.sx) < 4 && abs(my - it.sy) < 4)
            return; /* a click on the titlebar is not a move */
        it.moved = true;
        if (ww[w].snap != SNAP_NONE) {
            /* Pulled out of its snapped place: back to its own size, held at
             * the same point of the titlebar (proportionally). */
            rect f = ww[w].floating.w > 0 ? ww[w].floating : win->r;
            float fx = win->r.w > 0 ? (float)(it.sx - win->r.x) / (float)win->r.w : 0.5f;
            it.dx = (int)(fx * f.w);
            win->r.w = f.w, win->r.h = f.h;
            ww[w].snap = SNAP_NONE;
            win->maximized = false;
            tell_app(w);
            LOG("wm snap win-%d none\n", w);
        }
    }
    rect r = win->r;
    r.x = mx - it.dx, r.y = my - it.dy;
    /* the titlebar stays somewhere it can be grabbed again */
    if (r.y < 0) r.y = 0;
    if (r.y > A.H - 48) r.y = A.H - 48;
    if (r.x > A.W - 80) r.x = A.W - 80;
    if (r.x + r.w < 80) r.x = 80 - r.w;
    win->r = r;
    int t = snap_target(mx, my);
    float k = 1 - expf(-dt / 55.0f);
    if (t != SNAP_NONE) {
        rect to = snap_rect(t);
        if (it.preview_a < 0.02f)
            it.preview = scale_about_centre(win->r, 0.9f);
        it.preview = reduced() ? to : lerp_rect(it.preview, to, k);
        it.preview_a += (1 - it.preview_a) * (reduced() ? 1 : k);
    } else {
        it.preview_a -= it.preview_a * (reduced() ? 1 : k);
    }
    if (t != it.target)
        LOG("wm snap preview %s\n", snap_names[t]);
    it.target = t;
}

static void draw_preview(void) {
    if (it.mode != MODE_MOVE || it.preview_a < 0.02f)
        return;
    float a = it.preview_a, rad[4];
    frame_radii(rad, it.preview);
    float r0 = rad[0] > 6 ? rad[0] : 6;
    fill_round(A.frame, it.preview, r0, color_alpha(D.accent, 0.16f * a));
    fill_round(A.frame, rect_inset(it.preview, 6), r0 - 4 > 2 ? r0 - 4 : 2, RGBA(255, 255, 255, (int)(18 * a)));
    stroke_round(A.frame, it.preview, r0, 2, color_alpha(D.accent, 0.75f * a));
}

void wm_begin_move(int w) {
    if (w < 0 || w >= WIN_MAX || !A.win[w].open || it.mode)
        return;
    struct window *win = &A.win[w];
    win->z = ++A.ztop;
    int mx = A.in.mx, my = A.in.my;
    if (last_press.w == w && A.now - last_press.t < 450 && abs(mx - last_press.x) < 8 && abs(my - last_press.y) < 8) {
        last_press.w = -1; /* a double click on the titlebar */
        wm_toggle_maximize(w);
        return;
    }
    last_press.w = w, last_press.x = mx, last_press.y = my, last_press.t = A.now;
    it = (typeof(it)){.mode = MODE_MOVE, .win = w, .sx = mx, .sy = my, .start = win->r, .dx = mx - win->r.x,
                      .dy = my - win->r.y, .last_t = A.now, .was_free = ww[w].snap == SNAP_NONE};
    A.drag_win = w;
    A.drag_dx = it.dx, A.drag_dy = it.dy;
}

/* Which border of a window's rect the point is on (E_*), 0 inside, -1 away. */
static int edges_at(rect r, int x, int y) {
    if (!pt_in((rect){r.x - BORDER_OUT, r.y - BORDER_OUT, r.w + 2 * BORDER_OUT, r.h + 2 * BORDER_OUT}, x, y))
        return -1;
    int e = 0;
    if (x < r.x + BORDER_IN) e |= E_L;
    else if (x >= r.x + r.w - BORDER_IN) e |= E_R;
    if (y < r.y + BORDER_IN) e |= E_T;
    else if (y >= r.y + r.h - BORDER_IN) e |= E_B;
    if ((e & (E_L | E_R)) && !(e & (E_T | E_B)))
        e |= y < r.y + CORNER ? E_T : y >= r.y + r.h - CORNER ? E_B : 0;
    else if ((e & (E_T | E_B)) && !(e & (E_L | E_R)))
        e |= x < r.x + CORNER ? E_L : x >= r.x + r.w - CORNER ? E_R : 0;
    return e;
}

static struct switcher {
    bool open;
    int n, sel, steps;
    int list[WIN_MAX];
    rect tiles[WIN_MAX];
    uint64_t at;
    float hl; /* the highlight's x, sliding to the chosen tile */
} sw;

void wm_pointer(const int *order, int n, bool over_ui, int *top) {
    if (it.mode && (A.drag_win != it.win || it.win < 0 || !A.win[it.win].open || A.win[it.win].minimized))
        end_interaction();
    if (it.mode) {
        /* The pointer's last position counts even when the button came up in
         * the same frame: a quick drag must not stop short of where it ended. */
        if (it.mode == MODE_MOVE)
            update_move();
        else
            update_resize();
        if (!A.in.down)
            end_interaction();
        if (it.mode)
            *top = it.win;
        return;
    }
    int was = hover_edges;
    hover_win = -1, hover_edges = 0;
    if (!over_ui && !sw.open && !A.confirm_power)
        for (int i = n - 1; i >= 0; i--) {
            int w = order[i];
            int e = edges_at(A.win[w].r, A.in.mx, A.in.my);
            if (e < 0)
                continue;
            if (e > 0 && ww[w].snap != SNAP_MAX && !ww[w].anim)
                hover_win = w, hover_edges = e;
            break;
        }
    if (hover_edges != was)
        LOG("wm cursor %s%s%s%s\n", hover_edges ? "resize " : "normal", hover_edges & E_T ? "n" : hover_edges & E_B ? "s" : "",
            hover_edges & E_L ? "w" : hover_edges & E_R ? "e" : "", "");
    if (hover_win >= 0 && A.in.pressed) {
        start_resize(hover_win, hover_edges);
        A.in.pressed = false; /* the press was for the border, not what is under it */
        *top = hover_win;
    }
}

void wm_cursor(canvas *c, int x, int y) {
    int e = it.mode == MODE_RESIZE ? it.edges : hover_edges;
    if (!e || A.screen != SCR_DESKTOP)
        return;
    /* two small chevrons either side of the dot, pointing the ways it can go */
    float ux = e & E_L ? -1.0f : e & E_R ? 1.0f : 0, uy = e & E_T ? -1.0f : e & E_B ? 1.0f : 0;
    float len = sqrtf(ux * ux + uy * uy);
    ux /= len, uy /= len;
    float px = -uy, py = ux;
    for (int pass = 0; pass < 2; pass++) {
        float wdt = pass ? 1.8f : 4.0f;
        color col = pass ? RGBA(255, 255, 255, 245) : RGBA(20, 18, 24, 120);
        for (int s = -1; s <= 1; s += 2) {
            float tx = x + s * ux * 15, ty = y + s * uy * 15, bx = tx - s * ux * 5, by = ty - s * uy * 5;
            draw_line(c, bx + px * 5, by + py * 5, tx, ty, wdt, col);
            draw_line(c, bx - px * 5, by - py * 5, tx, ty, wdt, col);
        }
    }
}

/* ---- window events ---- */

void wm_opened(int w) {
    if (w < 0 || w >= WIN_MAX)
        return;
    memset(&ww[w], 0, sizeof ww[w]);
    A.win[w].opened_at = 0; /* desktop.c's own arrival glide gives way to the animation here */
    A.win[w].maximized = false;
    start_anim(w, ANIM_OPEN, A.win[w].r, reduced() ? 160 : 260);
    if (w >= WIN_COUNT && A.win[w].view)
        A.win[w].view->wm_state = 0;
}

void wm_unmapped(int w) {
    for (int i = 0; i < NGHOST; i++)
        if (ghosts[i].used && ghosts[i].waiting && ghosts[i].w == w) {
            ghosts[i].waiting = false; /* it closed: now it fades */
            ghosts[i].at = A.now;
        }
    if (it.win == w)
        end_interaction();
    ww[w].anim = ANIM_NONE;
    ww[w].pending_close = ww[w].pending_min = false;
}

void wm_close(int w) {
    if (w >= 0 && w < WIN_MAX && A.win[w].open)
        ww[w].pending_close = true;
}

void wm_minimize(int w) {
    if (w >= 0 && w < WIN_MAX && A.win[w].open && !A.win[w].minimized)
        ww[w].pending_min = true;
}

void wm_toggle_maximize(int w) {
    if (w >= 0 && w < WIN_MAX && A.win[w].open)
        set_snap(w, ww[w].snap == SNAP_MAX ? SNAP_NONE : SNAP_MAX);
}

void wm_activate(int w) {
    if (w < 0 || w >= WIN_MAX || !A.win[w].open)
        return;
    struct window *win = &A.win[w];
    if (win->minimized) {
        win->minimized = false;
        start_anim(w, ANIM_RESTORE, item_rect(w), reduced() ? 160 : 320);
    }
    win->z = ++A.ztop;
}

/* Does what was asked of a window once this frame's picture of it is kept. */
static void finish_pending(int w, const uint32_t *img, rect shown_at) {
    struct wmw *m = &ww[w];
    if (m->pending_min) {
        m->pending_min = false;
        rect to = item_rect(w);
        A.win[w].minimized = true;
        if (it.win == w)
            end_interaction();
        struct ghost *g = img ? new_ghost(w, img, shown_at) : NULL;
        if (g) {
            g->kind = GHOST_MIN;
            g->to = to;
            g->ms = reduced() ? 160 : 300;
        }
        LOG("wm minimized win-%d\n", w);
    }
    if (m->pending_close) {
        m->pending_close = false;
        if (it.win == w)
            end_interaction();
        struct ghost *g = img ? new_ghost(w, img, shown_at) : NULL;
        if (g) {
            g->kind = GHOST_CLOSE;
            g->ms = reduced() ? 140 : 190;
            g->waiting = w >= WIN_COUNT; /* an app may still ask to save first */
        }
        desktop_close_window(w);
        LOG("wm closed win-%d\n", w);
    }
}

void wm_draw_window(int w, bool focused_now, void (*draw)(int, bool)) {
    struct window *win = &A.win[w];
    struct wmw *m = &ww[w];
    if (!win->open || win->minimized)
        return;
    if (scene.using && w != scene.win && win->z < A.win[scene.win].z)
        return; /* already in the kept picture */
    if (interacting() && w == it.win) {
        if (!scene.using && !scene.valid)
            capture_scene();
        draw_preview();
    }
    rect dst;
    float alpha = 1;
    bool live = live_anim(w, &dst, &alpha);
    if (!live && !m->pending_close && !m->pending_min) {
        draw(w, focused_now);
        return;
    }
    static void *bk_buf, *img_buf;
    static size_t bk_cap, img_cap;
    rect r = win->r;
    rect R = on_screen((rect){r.x - MARGIN, r.y - MARGIN, r.w + 2 * MARGIN, r.h + 2 * MARGIN});
    uint32_t *bk = live ? grow(&bk_buf, &bk_cap, (size_t)R.w * R.h * 4 + 4) : NULL;
    uint32_t *img = r.w > 0 && r.h > 0 ? grow(&img_buf, &img_cap, (size_t)r.w * r.h * 4) : NULL;
    if (live && bk)
        save_region(R, bk);
    draw(w, focused_now);
    if (img)
        grab(r, img);
    if (live && bk && img) {
        restore_region(R, bk);
        float rad[4];
        frame_radii(rad, r);
        ghost_shadow(dst, r, alpha);
        composite(img, r.w, r.h, rad, dst, alpha);
    }
    finish_pending(w, img, live ? dst : r);
}

/* ---- keyboard ---- */

static void switcher_build(void) {
    sw.n = 0;
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open)
            sw.list[sw.n++] = w;
    /* most recently used first: the front window, then the one before it */
    for (int i = 1; i < sw.n; i++)
        for (int j = i; j > 0 && A.win[sw.list[j - 1]].z < A.win[sw.list[j]].z; j--) {
            int t = sw.list[j];
            sw.list[j] = sw.list[j - 1];
            sw.list[j - 1] = t;
        }
}

static void switcher_step(int dir) {
    if (!sw.open) {
        switcher_build();
        if (!sw.n)
            return;
        sw.open = true;
        sw.at = A.now;
        sw.sel = 0;
        sw.steps = 0;
        sw.hl = -1;
    }
    sw.sel = (sw.sel + dir + sw.n) % sw.n;
    sw.steps++;
    LOG("wm switcher %d of %d: win-%d\n", sw.sel + 1, sw.n, sw.list[sw.sel]);
}

static void switcher_commit(void) {
    if (!sw.open)
        return;
    sw.open = false;
    int w = sw.list[sw.sel];
    if (A.win[w].open) {
        wm_activate(w);
        LOG("wm switched to win-%d\n", w);
    }
}

static void super_arrow(int code) {
    int f = focused();
    if (f < 0)
        return;
    int s = ww[f].snap, to = s;
    switch (code) {
    case KEY_LEFT: to = s == SNAP_RIGHT ? SNAP_NONE : s == SNAP_TR ? SNAP_TL : s == SNAP_BR ? SNAP_BL : s == SNAP_TL || s == SNAP_BL ? s : SNAP_LEFT; break;
    case KEY_RIGHT: to = s == SNAP_LEFT ? SNAP_NONE : s == SNAP_TL ? SNAP_TR : s == SNAP_BL ? SNAP_BR : s == SNAP_TR || s == SNAP_BR ? s : SNAP_RIGHT; break;
    case KEY_UP: to = s == SNAP_LEFT ? SNAP_TL : s == SNAP_RIGHT ? SNAP_TR : s == SNAP_BL ? SNAP_LEFT : s == SNAP_BR ? SNAP_RIGHT : SNAP_MAX; break;
    case KEY_DOWN:
        if (s == SNAP_NONE) {
            wm_minimize(f);
            return;
        }
        to = s == SNAP_LEFT ? SNAP_BL : s == SNAP_RIGHT ? SNAP_BR : s == SNAP_TL ? SNAP_LEFT : s == SNAP_TR ? SNAP_RIGHT : SNAP_NONE;
        break;
    }
    if (to != s)
        set_snap(f, to);
}

static void drop_key(int k, int count) {
    int o = 0;
    for (int i = 0; i < A.in.nkeys; i++) {
        if (A.in.keys[i] == k && count > 0) {
            count--;
            continue;
        }
        A.in.keys[o++] = A.in.keys[i];
    }
    A.in.nkeys = o;
}

void wm_keys(void) {
    static bool alt, meta, meta_used;
    bool tap_meta = false;
    int tabs = 0, escs = 0, spaces = 0, arrows[4] = {0};
    int keep = 0;
    for (int i = 0; i < A.in.nraw; i++) {
        int code = A.in.raw[i].code;
        bool down = A.in.raw[i].down, take = false;
        switch (code) {
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
            alt = down;
            if (!down)
                switcher_commit();
            break;
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
            if (down)
                meta = true, meta_used = false;
            else if (meta) {
                meta = false;
                tap_meta = !meta_used; /* Super alone still opens what the design gave it */
            }
            break;
        case KEY_TAB:
            if (alt && !A.confirm_power) {
                take = true;
                if (down) {
                    switcher_step(A.in.shift ? -1 : 1);
                    tabs++;
                }
            }
            break;
        case KEY_F4:
            if (alt && !A.confirm_power) {
                take = true;
                if (down && focused() >= 0)
                    wm_close(focused());
            }
            break;
        case 57: /* KEY_SPACE: Alt+Space is search, whatever the design gave Super */
            if (alt && !A.confirm_power) {
                take = true;
                if (down) {
                    launcher_open(NULL);
                    spaces++;
                }
            }
            break;
        case KEY_ESC:
            if (sw.open && down) {
                sw.open = false;
                take = true;
                escs++;
            }
            break;
        case KEY_UP:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_DOWN:
            if (meta && !A.confirm_power) {
                take = true;
                meta_used = true;
                if (down) {
                    super_arrow(code);
                    arrows[code == KEY_UP ? 0 : code == KEY_LEFT ? 1 : code == KEY_RIGHT ? 2 : 3]++;
                }
            }
            break;
        default:
            if (meta && down)
                meta_used = true;
        }
        if (!take)
            A.in.raw[keep++] = A.in.raw[i];
    }
    A.in.nraw = keep;
    drop_key(K_META, 32); /* sent on release instead, when Super was pressed alone */
    drop_key(K_TAB, tabs);
    drop_key(K_ESC, escs);
    drop_key(' ', spaces);
    drop_key(K_UP, arrows[0]);
    drop_key(K_LEFT, arrows[1]);
    drop_key(K_RIGHT, arrows[2]);
    drop_key(K_DOWN, arrows[3]);
    if (tap_meta && A.in.nkeys < 32)
        A.in.keys[A.in.nkeys++] = K_META;
    if (sw.open) {
        /* the switcher has the pointer and keys while it is up */
        for (int i = 0; i < sw.n; i++)
            if (pt_in(sw.tiles[i], A.in.mx, A.in.my)) {
                if (A.in.moved && sw.sel != i) {
                    sw.sel = i;
                    sw.steps++;
                }
                if (A.in.clicked) {
                    sw.sel = i;
                    switcher_commit();
                }
            }
        A.in.pressed = A.in.clicked = A.in.right_clicked = false;
        A.in.wheel = 0;
        A.in.nkeys = 0;
    }
}

/* ---- the Alt+Tab switcher ---- */

/* The design's own icon look (shape and style), from an icon it drew. */
static const fm_style *icon_style_of(fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (!strcmp(n->tag, "icon") && n->app >= 0 && n->laid_out && n->st.icon_shape[0])
            return &n->st;
        const fm_style *s = icon_style_of(n->first);
        if (s)
            return s;
    }
    return NULL;
}

void wm_draw_overlay(void) {
    if (!sw.open)
        return;
    /* A quick Alt+Tab switches without the overlay flashing up. */
    float age = (float)(A.now - sw.at);
    if (age < 90 && sw.steps < 2)
        return;
    int n = 0;
    for (int i = 0; i < sw.n; i++)
        if (A.win[sw.list[i]].open)
            sw.list[n++] = sw.list[i];
    if (sw.sel >= n)
        sw.sel = n - 1;
    sw.n = n;
    if (!n) {
        sw.open = false;
        return;
    }
    float e = reduced() ? 1 : ease_out((age - 90) / 160.0f);
    int pad = 22, gap = 10, tw = 132, th = 128;
    if (n * tw + (n - 1) * gap + 2 * pad > A.W - 80)
        tw = (A.W - 80 - 2 * pad + gap) / n - gap;
    if (tw < 64)
        tw = 64;
    int pw = n * tw + (n - 1) * gap + 2 * pad, ph = th + 2 * pad + 40;
    rect panel = {(A.W - pw) / 2, (A.H - ph) / 2 + (int)((1 - e) * 12), pw, ph};
    float rad = D.radius + 8 > 28 ? 28 : D.radius + 8;
    bool dark = color_luma(D.surface) < 0.3f;

    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(RGBA(0, 0, 0, 255), (dark ? 0.22f : 0.12f) * e));
    shadow_round(A.frame, (rect){panel.x, panel.y + 18, panel.w, panel.h}, rad, 54, RGBA(0, 0, 0, (int)((dark ? 120 : 70) * e)));
    float op = D.surface_opacity < 0.8f ? 0.8f : D.surface_opacity;
    if (D.material != MAT_SOLID && D.blur >= 1) {
        blur_fast(A.frame, panel, rad, (int)(D.blur * 0.9f) + 6);
        vibrancy(A.frame, panel, rad, 1.35f, dark ? -4 : 6);
    } else {
        op = 1;
    }
    color top = color_mix(D.surface, RGBA(255, 255, 255, 255), dark ? 0.08f : 0.25f);
    fill_round_vgradient(A.frame, panel, rad, color_alpha(top, op * e), color_alpha(D.surface, op * e));
    stroke_round_vgradient(A.frame, panel, rad, 1, RGBA(255, 255, 255, (int)((dark ? 70 : 150) * e)),
                           RGBA(255, 255, 255, (int)((dark ? 8 : 30) * e)));

    /* the highlight slides to the chosen window */
    float want = (float)(panel.x + pad + sw.sel * (tw + gap));
    sw.hl = sw.hl < 0 || reduced() ? want : sw.hl + (want - sw.hl) * 0.45f;
    rect hl = {(int)lroundf(sw.hl), panel.y + pad, tw, th};
    fill_round(A.frame, hl, D.radius * 0.7f + 4, color_alpha(D.accent, 0.22f * e));
    stroke_round(A.frame, hl, D.radius * 0.7f + 4, 2, color_alpha(D.accent, 0.9f * e));

    const fm_style *ist = A.ui && A.ui->desktop ? icon_style_of(A.ui->desktop->first) : NULL;
    fm_style st;
    memset(&st, 0, sizeof st);
    if (ist) {
        memcpy(st.icon_shape, ist->icon_shape, sizeof st.icon_shape);
        memcpy(st.icon_style, ist->icon_style, sizeof st.icon_style);
    }
    st.opacity = 1;
    font *f = font_get(D.font_body, 13 * (A.prof.text_scale > 0.5f ? A.prof.text_scale : 1));
    for (int i = 0; i < n; i++) {
        int w = sw.list[i], idx = open_index(w);
        rect t = {panel.x + pad + i * (tw + gap), panel.y + pad, tw, th};
        sw.tiles[i] = t;
        int is = tw - 52 < 64 ? (tw - 52 > 36 ? tw - 52 : 36) : 64;
        rect ic = {t.x + (tw - is) / 2, t.y + 16, is, is};
        fm_host_draw_icon(ic, fm_host_window_app(idx), &st, i == sw.sel);
        if (A.win[w].minimized) /* minimized ones look tucked away */
            fill_round(A.frame, rect_inset(ic, -2), is * 0.28f, color_alpha(D.surface, 0.45f * e));
        char title[160];
        snprintf(title, sizeof title, "%s", fm_host_window_title(idx));
        int maxw = tw - 14, len = (int)strlen(title);
        if (text_width(f, title) > maxw) {
            while (len > 0 && text_width_n(f, title, len) + text_width(f, "…") > maxw)
                len--;
            while (len > 0 && (title[len] & 0xC0) == 0x80)
                len--;
            snprintf(title + len, sizeof title - (size_t)len, "…");
        }
        draw_text_centered(A.frame, f, (rect){t.x + 4, ic.y + is + 10, tw - 8, 24}, title,
                           color_alpha(i == sw.sel ? D.surface_text : D.muted, e));
    }
    /* the chosen window's whole name, larger, under the row */
    font *hf = font_get(D.font_heading, 17 * (A.prof.text_scale > 0.5f ? A.prof.text_scale : 1));
    draw_text_centered(A.frame, hf, (rect){panel.x + pad, panel.y + pad + th + 6, pw - 2 * pad, 30},
                       fm_host_window_title(open_index(sw.list[sw.sel])), color_alpha(D.surface_text, e));
    static uint64_t logged;
    if (A.now - logged > 400) {
        logged = A.now;
        LOGRECT("wm-switcher", panel);
    }
}

/* ---- apps asking for things (xdg_toplevel requests) ---- */

void wl_host_view_request(wl_view *v, int req, uint32_t edges) {
    int w = (int)(intptr_t)v->host - 1;
    if (w < WIN_COUNT || w >= WIN_MAX || A.win[w].view != v)
        return;
    switch (req) {
    case WLS_REQ_MOVE:
        if (A.in.down && !it.mode)
            wm_begin_move(w);
        break;
    case WLS_REQ_RESIZE:
        if (A.in.down && !it.mode) {
            int e = (edges & 1 ? E_T : 0) | (edges & 2 ? E_B : 0) | (edges & 4 ? E_L : 0) | (edges & 8 ? E_R : 0);
            if (e)
                start_resize(w, e);
        }
        break;
    case WLS_REQ_MAXIMIZE:
        if (ww[w].snap != SNAP_MAX)
            set_snap(w, SNAP_MAX);
        break;
    case WLS_REQ_UNMAXIMIZE:
        if (ww[w].snap == SNAP_MAX)
            set_snap(w, SNAP_NONE);
        break;
    case WLS_REQ_MINIMIZE: wm_minimize(w); break;
    }
}

/* ---- frame timing ---- */

/* While a window is being moved or resized, every frame's work time and the
 * time between frames are kept, and summed up in the log when it ends. */
static struct {
    uint64_t begin, last_end;
    int frames;
    uint64_t work_sum, work_max, gap_sum, gap_max;
    bool active;
} ft;

void wm_frame_begin(void) { ft.begin = usec(); }

static bool animating(void) {
    for (int w = 0; w < WIN_MAX; w++)
        if (ww[w].anim)
            return true;
    for (int i = 0; i < NGHOST; i++)
        if (ghosts[i].used && !ghosts[i].waiting)
            return true;
    return it.mode == MODE_MOVE && it.preview_a > 0.02f;
}

int wm_poll_ms(int idle_ms) {
    if (A.screen != SCR_DESKTOP || !(interacting() || sw.open || animating()))
        return idle_ms;
    /* aim for a frame every 16 ms, counting the time this one took */
    int spent = (int)((usec() - ft.begin) / 1000);
    return spent >= 15 ? 1 : 16 - spent;
}

/* Tells tests where the windows are (only when they settle: the serial
 * port is slow, and a log line per frame would itself make dragging stutter). */
static void log_windows(void) {
    static uint64_t last;
    static rect logged[WIN_MAX];
    static bool was_shown[WIN_MAX];
    if (A.now - last < 250 || interacting())
        return;
    last = A.now;
    for (int w = 0; w < WIN_MAX; w++) {
        bool vis = A.win[w].open && !A.win[w].minimized;
        rect r = A.win[w].r;
        if (vis == was_shown[w] && (!vis || !memcmp(&r, &logged[w], sizeof r)))
            continue;
        was_shown[w] = vis, logged[w] = r;
        char name[16];
        snprintf(name, sizeof name, "win-%d", w);
        if (vis)
            LOGRECT(name, r);
        else
            LOG("rect %s 0 0 0 0\n", name);
    }
}

void wm_frame_end(void) {
    uint64_t end = usec(), wk = end - ft.begin;
    if (interacting()) {
        if (!ft.active) {
            ft = (typeof(ft)){.begin = ft.begin, .active = true};
        } else {
            uint64_t gap = end - ft.last_end;
            ft.gap_sum += gap;
            if (gap > ft.gap_max)
                ft.gap_max = gap;
        }
        ft.frames++;
        ft.work_sum += wk;
        if (wk > ft.work_max)
            ft.work_max = wk;
    } else if (ft.active) {
        ft.active = false;
        if (ft.frames > 1)
            LOG("wm frames %d: work avg %.1f ms, worst %.1f ms; frame interval avg %.1f ms, worst %.1f ms\n", ft.frames,
                ft.work_sum / 1000.0 / ft.frames, ft.work_max / 1000.0, ft.gap_sum / 1000.0 / (ft.frames - 1),
                ft.gap_max / 1000.0);
    }
    ft.last_end = end;
    if (A.screen != SCR_DESKTOP)
        return;
    /* Asked of a window that wasn't drawn this frame: done without a picture. */
    for (int w = 0; w < WIN_MAX; w++)
        if (ww[w].pending_min || ww[w].pending_close)
            finish_pending(w, NULL, A.win[w].r);
    /* Apps draw themselves as the active window or not (title bars dim). */
    static int last_focus = -2;
    int f = focused();
    if (f != last_focus) {
        int pair[2] = {last_focus, f};
        for (int i = 0; i < 2; i++) {
            int w = pair[i];
            if (w >= WIN_COUNT && w < WIN_MAX && A.win[w].view && A.win[w].fitted && A.win[w].asked_w > 0)
                wls_configure(A.win[w].view, A.win[w].asked_w, A.win[w].asked_h, w == f);
        }
        last_focus = f;
    }
    log_windows();
}
