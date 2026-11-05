#define _GNU_SOURCE
#include "fmi.h"
#include "../iconart.h"
#include "../textedit.h"
#include <time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Drawing: every element is a box with an optional backdrop blur (glass),
 * vibrancy, shadow, glow, gradient fill and border, then its content. */

extern color readable_on(color bg);
extern void icon_symbol(canvas *c, rect box, int app, float stroke, color col);

/* ---- animation ---- */

static float approach(float cur, float target, float k) { return cur + (target - cur) * k; }

/* During a morph, every box (and the text in it) glides from where it was
 * drawn toward where layout put it this frame. Otherwise it simply is there. */
static void glide(fm_node *n, float k, bool morphing) {
    for (; n; n = n->next) {
        if (!n->av.vinit || !morphing) {
            n->av.vx = (float)n->vis.x, n->av.vy = (float)n->vis.y;
            n->av.vw = (float)n->vis.w, n->av.vh = (float)n->vis.h;
            n->av.vinit = true;
        } else {
            n->av.vx = approach(n->av.vx, (float)n->vis.x, k);
            n->av.vy = approach(n->av.vy, (float)n->vis.y, k);
            n->av.vw = approach(n->av.vw, (float)n->vis.w, k);
            n->av.vh = approach(n->av.vh, (float)n->vis.h, k);
            n->vis = (rect){(int)lroundf(n->av.vx), (int)lroundf(n->av.vy), (int)lroundf(n->av.vw),
                            (int)lroundf(n->av.vh)};
        }
        if (n->kind == FM_ELEM)
            glide(n->first, k, morphing);
    }
}

bool fm_reduce_motion;

static void animate_node(fm_node *n, float dt, bool morphing) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        float k = n->st.transition <= 1 ? 1 : 1 - expf(-dt / (n->st.transition * 0.35f));
        if (morphing) /* one version of the desktop turning into another */
            k = 1 - expf(-dt / 150.0f);
        float kf = 1 - expf(-dt / 60.0f), ko = 1 - expf(-dt / 70.0f);
        if (fm_reduce_motion) /* (straight to where it is going) */
            k = kf = ko = 1;
        n->a_hover = approach(n->a_hover, n->hover ? 1 : 0, kf);
        n->a_press = approach(n->a_press, n->active ? 1 : 0, kf);
        n->a_open = approach(n->a_open, n->open ? 1 : 0, ko);
        if (fabsf(n->a_open - (n->open ? 1 : 0)) < 0.01f)
            n->a_open = n->open ? 1 : 0;
        if (n->flash > 0)
            n->flash = fmaxf(0, n->flash - dt / 1400.0f);
        const fm_style *s = &n->st;
        if (!n->av.init) {
            n->av.bg = s->bg;
            n->av.color = s->color;
            n->av.border = s->border;
            n->av.glow = s->glow;
            n->av.shadow = s->shadow;
            n->av.glow_size = s->glow_size;
            n->av.scale = s->scale;
            n->av.opacity = s->opacity;
            n->av.tx = s->tx;
            n->av.ty = s->ty;
            n->av.init = true;
            if (n->fresh && morphing) { /* new in this version: it arrives */
                n->av.opacity = 0;
                n->av.scale = s->scale * 0.96f;
            }
            n->fresh = false;
        } else {
            if (n->av.bg.kind != s->bg.kind) {
                /* fade from transparent when the kind changes */
                if (n->av.bg.kind == FM_BG_NONE) {
                    n->av.bg = s->bg;
                    n->av.bg.a = color_alpha(s->bg.a, 0);
                    n->av.bg.b = color_alpha(s->bg.b, 0);
                } else if (s->bg.kind == FM_BG_NONE) {
                    n->av.bg.a = color_mix(n->av.bg.a, color_alpha(n->av.bg.a, 0), k);
                    n->av.bg.b = color_mix(n->av.bg.b, color_alpha(n->av.bg.b, 0), k);
                    if (C_A(n->av.bg.a) < 3 && C_A(n->av.bg.b) < 3)
                        n->av.bg.kind = FM_BG_NONE;
                    goto rest;
                } else {
                    n->av.bg.kind = s->bg.kind;
                }
            }
            n->av.bg.a = color_mix(n->av.bg.a, s->bg.a, k);
            n->av.bg.b = color_mix(n->av.bg.b, s->bg.b, k);
            n->av.bg.angle = s->bg.angle;
            if (n->av.bg.nstops == s->bg.nstops) { /* the stops ease too */
                for (int i = 0; i < s->bg.nstops; i++) {
                    n->av.bg.stop_c[i] = color_mix(n->av.bg.stop_c[i], s->bg.stop_c[i], k);
                    n->av.bg.stop_p[i] = approach(n->av.bg.stop_p[i], s->bg.stop_p[i], k);
                }
            } else {
                n->av.bg.nstops = s->bg.nstops;
                memcpy(n->av.bg.stop_c, s->bg.stop_c, sizeof s->bg.stop_c);
                memcpy(n->av.bg.stop_p, s->bg.stop_p, sizeof s->bg.stop_p);
            }
            n->av.bg.repeating = s->bg.repeating, n->av.bg.circle = s->bg.circle;
            n->av.bg.cx = s->bg.cx, n->av.bg.cy = s->bg.cy;
        rest:
            n->av.color = color_mix(n->av.color, s->color, k);
            n->av.border = color_mix(n->av.border, s->border, k);
            n->av.glow = color_mix(n->av.glow, s->glow, k);
            n->av.shadow = color_mix(n->av.shadow, s->shadow, k);
            n->av.glow_size = approach(n->av.glow_size, s->glow_size, k);
            n->av.scale = approach(n->av.scale, s->scale, k);
            n->av.opacity = approach(n->av.opacity, s->opacity, k);
            n->av.tx = approach(n->av.tx, s->tx, k);
            n->av.ty = approach(n->av.ty, s->ty, k);
        }
        animate_node(n->first, dt, morphing);
    }
}

void fm_animate(fm_doc *d, float dt_ms) {
    if (dt_ms > 200)
        dt_ms = 200;
    d->clock += dt_ms;
    bool morphing = d->morph_until > d->clock;
    if (fm_reduce_motion)
        morphing = false;
    if (d->desktop) {
        animate_node(d->desktop, dt_ms, morphing);
        glide(d->desktop, fm_reduce_motion ? 1 : 1 - expf(-dt_ms / 150.0f), morphing);
    }
}

void fm_entrance(fm_doc *d) { d->entrance_at = d->clock; }

/* ---- one version of a desktop becoming another ---- */

static const fm_node *find_id(const fm_node *n, const char *id) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        if (!strcmp(n->id, id))
            return n;
        const fm_node *f = find_id(n->first, id);
        if (f)
            return f;
    }
    return NULL;
}

static bool authored(const fm_node *n) { return n->id[0] && n->id[0] != '_'; }

/* The k-th child of `parent` with this tag and no id of its own. */
static const fm_node *nth_like(const fm_node *parent, const fm_node *like, int k) {
    for (const fm_node *c = parent ? parent->first : NULL; c; c = c->next)
        if (c->kind == FM_ELEM && !authored(c) && !strcmp(c->tag, like->tag) && c->app == like->app && k-- == 0)
            return c;
    return NULL;
}

static void morph_walk(const fm_doc *from, const fm_node *oparent, fm_node *nparent) {
    for (fm_node *c = nparent->first; c; c = c->next) {
        if (c->kind != FM_ELEM)
            continue;
        const fm_node *o = authored(c) ? find_id(from->desktop, c->id) : NULL;
        if (!o && !authored(c)) {
            int k = 0;
            for (const fm_node *p = nparent->first; p != c; p = p->next)
                if (p->kind == FM_ELEM && !authored(p) && !strcmp(p->tag, c->tag) && p->app == c->app)
                    k++;
            o = nth_like(oparent, c, k);
        }
        if (o && o->av.init) {
            c->av = o->av; /* starts where and how it was */
            c->fresh = false;
            const fm_node *ot = o->first; /* and its words with it */
            for (fm_node *t = c->first; t; t = t->next) {
                if (t->kind != FM_TEXT)
                    continue;
                while (ot && ot->kind != FM_TEXT)
                    ot = ot->next;
                if (!ot)
                    break;
                t->av = ot->av;
                ot = ot->next;
            }
        } else {
            c->av.init = c->av.vinit = false;
            c->fresh = true;
        }
        morph_walk(from, o, c);
    }
}

void fm_morph_from(fm_doc *to, const fm_doc *from, float ms) {
    if (!to || !from || !to->desktop || !from->desktop)
        return;
    if (from->desktop->av.init)
        to->desktop->av = from->desktop->av;
    morph_walk(from, from->desktop, to->desktop);
    to->entrance_at = -1;
    to->morph_until = to->clock + ms;
}

/* How far surface number i has arrived (0..1), eased. */
static float arrival(fm_doc *d, int i) {
    if (d->entrance_at < 0)
        return 1;
    float t = (d->clock - d->entrance_at - 120 - (float)i * 70) / 420;
    if (t >= 1) {
        if (i == 0 && d->clock - d->entrance_at > 4000)
            d->entrance_at = -1;
        return 1;
    }
    if (t <= 0)
        return 0;
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

/* ---- fills ---- */

struct grad {
    fm_bg bg;
    rect r;
    float op;
    float dx, dy, len;
};

/* The colour t of the way along a gradient's stops (repeating ones wrap round their last stop) */
static color stop_at(const fm_bg *bg, float t) {
    if (bg->nstops < 2)
        return bg->nstops ? bg->stop_c[0] : color_mix(bg->a, bg->b, t);
    if (bg->repeating) {
        float span = bg->stop_p[bg->nstops - 1] - bg->stop_p[0];
        if (span > 1e-4f) {
            t = fmodf(t - bg->stop_p[0], span);
            if (t < 0)
                t += span;
            t += bg->stop_p[0];
        }
    }
    if (t <= bg->stop_p[0])
        return bg->stop_c[0];
    for (int i = 1; i < bg->nstops; i++)
        if (t <= bg->stop_p[i]) {
            float d = bg->stop_p[i] - bg->stop_p[i - 1];
            return d < 1e-5f ? bg->stop_c[i] : color_mix(bg->stop_c[i - 1], bg->stop_c[i], (t - bg->stop_p[i - 1]) / d);
        }
    return bg->stop_c[bg->nstops - 1];
}

static color shade_grad(void *ctx, int x, int y) {
    struct grad *g = ctx;
    float t;
    float cx = g->r.x + g->r.w * (g->bg.nstops ? g->bg.cx : 0.5f), cy = g->r.y + g->r.h * (g->bg.nstops ? g->bg.cy : 0.5f);
    if (g->bg.kind == FM_BG_RADIAL) {
        /* the farthest corner is the end, as CSS's default */
        float fx = fmaxf(cx - g->r.x, g->r.x + g->r.w - cx), fy = fmaxf(cy - g->r.y, g->r.y + g->r.h - cy);
        if (!g->bg.nstops)
            fx = g->r.w / 2.0f, fy = g->r.h / 2.0f;
        float rx = (x + 0.5f - cx), ry = (y + 0.5f - cy);
        if (g->bg.circle) {
            float R = sqrtf(fx * fx + fy * fy);
            t = sqrtf(rx * rx + ry * ry) / (R > 0 ? R : 1);
        } else {
            rx /= fx > 0 ? fx * 1.41421f : 1, ry /= fy > 0 ? fy * 1.41421f : 1;
            if (!g->bg.nstops)
                rx *= 1.41421f, ry *= 1.41421f;
            t = sqrtf(rx * rx + ry * ry);
        }
    } else if (g->bg.kind == FM_BG_CONIC) {
        float a = atan2f(x + 0.5f - cx, -(y + 0.5f - cy)) * 57.29578f - g->bg.angle; /* 0 = up, clockwise */
        a = fmodf(a, 360);
        if (a < 0)
            a += 360;
        t = a / 360;
    } else {
        float px = x + 0.5f - (g->r.x + g->r.w / 2.0f), py = y + 0.5f - (g->r.y + g->r.h / 2.0f);
        t = 0.5f + (px * g->dx + py * g->dy) / g->len;
    }
    if (!g->bg.repeating) {
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    return color_alpha(stop_at(&g->bg, t), g->op);
}

static color shade_one(void *ctx, int x, int y) {
    (void)x, (void)y;
    return *(color *)ctx;
}

static float corner_r(const float rad[4], rect r) {
    float m = fminf((float)r.w, (float)r.h) / 2;
    float v = rad[0];
    return v > m ? m : v;
}

static bool same_radii(const float rad[4]) { return rad[0] == rad[1] && rad[1] == rad[2] && rad[2] == rad[3]; }

/* Rounded rectangle with a different radius per corner. */
static void fill_rr(canvas *c, rect r, const float rad_in[4], shade_fn shade, void *ctx) {
    if (same_radii(rad_in)) {
        fill_round_shaded(c, r, corner_r(rad_in, r), shade, ctx);
        return;
    }
    float m = fminf((float)r.w, (float)r.h) / 2, rad[4];
    for (int i = 0; i < 4; i++)
        rad[i] = fminf(fmaxf(rad_in[i], 0), m);
    int x0 = r.x < c->cx0 ? c->cx0 : r.x, y0 = r.y < c->cy0 ? c->cy0 : r.y;
    int x1 = r.x + r.w > c->cx1 ? c->cx1 : r.x + r.w, y1 = r.y + r.h > c->cy1 ? c->cy1 : r.y + r.h;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f, cov = 1;
            bool left = px < r.x + r.w / 2.0f, top = py < r.y + r.h / 2.0f;
            float rr = top ? (left ? rad[0] : rad[1]) : (left ? rad[3] : rad[2]);
            if (rr > 0) {
                float cx = left ? r.x + rr : r.x + r.w - rr, cy = top ? r.y + rr : r.y + r.h - rr;
                bool in_corner = (left ? px < cx : px > cx) && (top ? py < cy : py > cy);
                if (in_corner) {
                    float dist = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
                    cov = rr + 0.5f - dist;
                    if (cov <= 0)
                        continue;
                    if (cov > 1)
                        cov = 1;
                }
            }
            color col = shade(ctx, x, y);
            uint32_t a = (uint32_t)(C_A(col) * cov);
            if (!a)
                continue;
            uint32_t ia = 255 - a, d0 = row[x];
            row[x] = 0xff000000u | ((C_R(col) * a + C_R(d0) * ia) / 255) << 16 | ((C_G(col) * a + C_G(d0) * ia) / 255) << 8 |
                     ((C_B(col) * a + C_B(d0) * ia) / 255);
        }
    }
}

/* ---- a background picture ---- */

struct bgpic {
    const uint32_t *px;
    int iw, ih;
    float x0, y0, w, h; /* where one copy of it lands */
    char repeat;
    bool pixelated;
    float op;
};

static color shade_pic(void *ctx, int x, int y) {
    struct bgpic *p = ctx;
    float u = (x + 0.5f - p->x0) / p->w, v = (y + 0.5f - p->y0) / p->h;
    if (p->repeat == 'r' || p->repeat == 'x') u -= floorf(u);
    if (p->repeat == 'r' || p->repeat == 'y') v -= floorf(v);
    if (u < 0 || v < 0 || u >= 1 || v >= 1)
        return 0;
    float fx = u * p->iw - 0.5f, fy = v * p->ih - 0.5f;
    if (p->pixelated || (p->w >= p->iw * 0.98f && p->w <= p->iw * 1.02f)) {
        int ix = (int)(u * p->iw), iy = (int)(v * p->ih);
        uint32_t c = p->px[(size_t)(iy < p->ih ? iy : p->ih - 1) * (size_t)p->iw + (size_t)(ix < p->iw ? ix : p->iw - 1)];
        return color_alpha(c, p->op);
    }
    int ix = (int)floorf(fx), iy = (int)floorf(fy);
    float tx = fx - ix, ty = fy - iy, acc[4] = {0};
    for (int q = 0; q < 4; q++) {
        int qx = ix + (q & 1), qy = iy + (q >> 1);
        if (p->repeat == 'r' || p->repeat == 'x') qx = (qx % p->iw + p->iw) % p->iw;
        if (p->repeat == 'r' || p->repeat == 'y') qy = (qy % p->ih + p->ih) % p->ih;
        qx = qx < 0 ? 0 : qx >= p->iw ? p->iw - 1 : qx;
        qy = qy < 0 ? 0 : qy >= p->ih ? p->ih - 1 : qy;
        uint32_t c = p->px[(size_t)qy * (size_t)p->iw + (size_t)qx];
        float w = ((q & 1) ? tx : 1 - tx) * ((q >> 1) ? ty : 1 - ty), a = C_A(c) / 255.0f * w;
        acc[0] += a, acc[1] += C_R(c) * a, acc[2] += C_G(c) * a, acc[3] += C_B(c) * a;
    }
    if (acc[0] < 0.002f)
        return 0;
    return RGBA((int)(acc[1] / acc[0]), (int)(acc[2] / acc[0]), (int)(acc[3] / acc[0]), (int)(acc[0] * 255 * p->op));
}

static void paint_pic(canvas *c, rect r, const float rad[4], const fm_style *s, float op) {
    int iw, ih;
    const uint32_t *px = fmi_picture(s->bg_img, r.w, r.h, &iw, &ih);
    if (!px || iw <= 0 || ih <= 0)
        return;
    float w = (float)iw, h = (float)ih;
    switch (s->bg_size) {
    case 'c': { float k = fmaxf((float)r.w / iw, (float)r.h / ih); w = iw * k, h = ih * k; break; }
    case 'C': { float k = fminf((float)r.w / iw, (float)r.h / ih); w = iw * k, h = ih * k; break; }
    case 'p': w = s->bg_sw, h = s->bg_sh > 0 ? s->bg_sh : ih * s->bg_sw / iw; break;
    case 'f': w = r.w * s->bg_sw, h = s->bg_sh > 0 ? r.h * s->bg_sh : ih * w / iw; break;
    }
    if (w < 1 || h < 1)
        return;
    struct bgpic p = {px, iw, ih, r.x + (r.w - w) * s->bg_px, r.y + (r.h - h) * s->bg_py, w, h,
                      s->bg_size == 'c' || s->bg_size == 'C' ? 'n' : s->bg_repeat, s->pixelated, op};
    fill_rr(c, r, rad, shade_pic, &p);
}

static void paint_bg(canvas *c, rect r, const float rad[4], fm_bg bg, float op) {
    if (bg.kind == FM_BG_NONE || r.w <= 0 || r.h <= 0)
        return;
    if (bg.kind == FM_BG_SOLID) {
        color col = color_alpha(bg.a, op);
        if (C_A(col) && same_radii(rad))
            fill_round(c, r, corner_r(rad, r), col); /* the same pixels, without a call per pixel */
        else if (C_A(col))
            fill_rr(c, r, rad, shade_one, &col);
        return;
    }
    struct grad g = {bg, r, op, 0, 0, 1};
    float a = (bg.angle - 90) * (float)M_PI / 180; /* CSS: 180deg = downwards */
    g.dx = cosf(a);
    g.dy = sinf(a);
    if (bg.kind == FM_BG_LINEAR && fabsf(g.dx) < 1e-3f && same_radii(rad) && bg.nstops <= 2 && !bg.repeating &&
        (bg.nstops < 2 || (bg.stop_p[0] <= 0 && bg.stop_p[1] >= 1))) { /* straight down or up: one colour a row */
        color top = color_alpha(bg.a, op), bottom = color_alpha(bg.b, op);
        fill_round_vgradient(c, r, corner_r(rad, r), g.dy > 0 ? top : bottom, g.dy > 0 ? bottom : top);
        return;
    }
    g.len = fabsf(r.w * g.dx) + fabsf(r.h * g.dy);
    if (g.len < 1)
        g.len = 1;
    fill_rr(c, r, rad, shade_grad, &g);
}

/* ---- clipping ---- */

typedef struct {
    int x0, y0, x1, y1;
} clipbox;

static clipbox clip_push(canvas *c, rect r) {
    clipbox old = {c->cx0, c->cy0, c->cx1, c->cy1};
    int x0 = r.x > c->cx0 ? r.x : c->cx0, y0 = r.y > c->cy0 ? r.y : c->cy0;
    int x1 = r.x + r.w < c->cx1 ? r.x + r.w : c->cx1, y1 = r.y + r.h < c->cy1 ? r.y + r.h : c->cy1;
    c->cx0 = x0, c->cy0 = y0, c->cx1 = x1 > x0 ? x1 : x0, c->cy1 = y1 > y0 ? y1 : y0;
    return old;
}

static void clip_pop(canvas *c, clipbox b) { c->cx0 = b.x0, c->cy0 = b.y0, c->cx1 = b.x1, c->cy1 = b.y1; }

/* ---- text ---- */

float fm_contrast_floor = 4.5f; /* WCAG AA; 7 when they asked for strong contrast */
/* The design lint (lint.c) looks at each run of text just before it is
 * drawn, with what is behind it already painted. NULL normally. */
void (*fm_text_probe)(canvas *c, fm_node *t, rect r, color col, float px);

/* The lightest and darkest of what's already drawn under `r`, and the
 * average: text has to read against all of it, not just the mean. */
static void sample_under(canvas *c, rect r, color *lo, color *hi, color *avg) {
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    float llo = 2, lhi = -1;
    *lo = *hi = *avg = RGBA(0, 0, 0, 255);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 10; i++) {
            int x = r.x + (r.w * (2 * i + 1)) / 20, y = r.y + (r.h * (2 * j + 1)) / 8;
            if (x < 0 || y < 0 || x >= c->w || y >= c->h)
                continue;
            uint32_t p = c->px[(size_t)y * c->w + x] | 0xff000000u;
            float l = color_luma(p);
            if (l < llo) llo = l, *lo = p;
            if (l > lhi) lhi = l, *hi = p;
            sr += C_R(p), sg += C_G(p), sb += C_B(p), n++;
        }
    if (n)
        *avg = RGBA(sr / n, sg / n, sb / n, 255);
}

static float worst_contrast(color fg, color lo, color hi) {
    float a = color_contrast(fg, lo), b = color_contrast(fg, hi);
    return a < b ? a : b;
}

/* Text stays readable whatever the AI chose. If its colour doesn't reach
 * the contrast floor against the worst spot behind it, the nearest
 * readable colour is used; if nothing can (a busy background with both
 * very light and very dark parts), a soft veil goes behind the words. */
static color guard(canvas *c, rect r, color col, float px) {
    color lo, hi, avg;
    sample_under(c, r, &lo, &hi, &avg);
    color solid = RGBA(C_R(col), C_G(col), C_B(col), 255);
    bool large = px >= 24;
    float need = fm_contrast_floor > 4.6f ? fm_contrast_floor : large || C_A(col) < 160 ? 3.0f : fm_contrast_floor;
    if (worst_contrast(solid, lo, hi) >= need)
        return col;
    /* Keep the hue, push lightness away from the background. */
    bool dark_bg = color_luma(avg) < 0.45f;
    color target = dark_bg ? RGBA(255, 255, 255, 255) : RGBA(0, 0, 0, 255);
    for (int k = 1; k <= 10; k++) {
        color t = color_mix(solid, target, k * 0.1f);
        if (worst_contrast(t, lo, hi) >= need)
            return RGBA(C_R(t), C_G(t), C_B(t), C_A(col) > 120 ? C_A(col) : 220);
    }
    /* Nothing reaches the floor. A veil behind the words is a last resort,
     * for text that would truly be lost (under about 2:1, e.g. on a busy
     * wallpaper); a designer's white on a mid-tone bar reads fine and keeps
     * its look, so it gets the strongest colour without the pill. */
    color best = RGBA(C_R(target), C_G(target), C_B(target), 255);
    if (worst_contrast(best, lo, hi) >= 2.0f || worst_contrast(solid, avg, avg) >= 3.0f)
        return worst_contrast(solid, avg, avg) >= 3.0f ? col : best;
    rect v = {r.x - 8, r.y - 3, r.w + 16, r.h + 6};
    fill_round(c, v, (float)v.h / 2, dark_bg ? RGBA(0, 0, 0, 110) : RGBA(255, 255, 255, 150));
    return best;
}

/* A line of text turned a quarter (or half) way: drawn white on a canvas of its own, its coverage then
 * laid down in the text's colour, turned. Starts where reading starts (top for 90, bottom for -90). */
static void draw_text_turned(canvas *c, font *f, rect r, const char *buf, color col, const fm_style *st, float s) {
    int rot = st->text_rot;
    int tw = text_width(f, buf), lh = (int)(font_line_height(f) * st->line_height);
    if (tw <= 0 || lh <= 0)
        return;
    canvas *m = canvas_new(tw + 2, lh + 2);
    if (!m)
        return;
    fm_style plain = *st;
    plain.tshadow = 0;
    plain.text_align = FM_T_START;
    plain.text_rot = 0;
    fmi_draw_line_in(m, f, (rect){1, 1, tw, lh}, buf, 0xffffffffu, &plain, s);
    bool side = rot == 90 || rot == -90;
    int W = side ? m->h : m->w, H = side ? m->w : m->h;
    int ox = r.x + (r.w - W) / 2, oy = r.y;
    int along = side ? r.h - H : r.w - W; /* room along the line, used as text-align says */
    if (along > 0) {
        int shift = st->text_align == FM_T_CENTER ? along / 2 : st->text_align == FM_T_END ? along : 0;
        if (rot == -90 || rot == 180)
            shift = along - shift; /* reading starts at the far end */
        if (side)
            oy += shift;
        else
            ox = r.x + shift;
    } else if (!side) {
        ox = r.x;
    }
    if (!side)
        oy = r.y + (r.h - H) / 2;
    float ca = C_A(col) / 255.0f;
    for (int y = 0; y < m->h; y++)
        for (int x = 0; x < m->w; x++) {
            uint32_t p = m->px[(size_t)y * (size_t)m->w + (size_t)x];
            float cov = (float)C_R(p) / 255.0f * ca;
            if (cov <= 0.004f)
                continue;
            int dx, dy;
            if (rot == 90)
                dx = ox + (m->h - 1 - y), dy = oy + x;
            else if (rot == -90)
                dx = ox + y, dy = oy + (m->w - 1 - x);
            else
                dx = ox + (m->w - 1 - x), dy = oy + (m->h - 1 - y);
            if (dx < c->cx0 || dy < c->cy0 || dx >= c->cx1 || dy >= c->cy1)
                continue;
            uint32_t *d = &c->px[(size_t)dy * (size_t)c->w + (size_t)dx];
            *d = RGBA((int)(C_R(col) * cov + C_R(*d) * (1 - cov)), (int)(C_G(col) * cov + C_G(*d) * (1 - cov)),
                      (int)(C_B(col) * cov + C_B(*d) * (1 - cov)), (int)C_A(*d) > (int)(cov * 255) ? (int)C_A(*d) : (int)(cov * 255));
        }
    canvas_free(m);
}

uint64_t fmi_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

struct text_edit *fmi_editor(fm_node *n) {
    if (!n->ed) {
        n->ed = calloc(1, sizeof *n->ed);
        const char *v = fm_attr_get(n, "value");
        te_reset(n->ed, v ? v : "");
    }
    return n->ed;
}

bool fmi_input_geom(fm_node *n, font **f, rect *box, int *tx, int *ty) {
    fm_node *tn = NULL;
    for (fm_node *k = n->first; k; k = k->next)
        if (k->kind == FM_TEXT && k->generated)
            tn = k;
    if (!tn)
        return false;
    const fm_style *s = &n->st;
    float sc = n->vscale > 0 ? n->vscale : 1;
    *f = fmi_text_font(s, tn->vscale > 0 ? tn->vscale : sc);
    int x1 = n->vis.x + n->vis.w - (int)((s->pad[1] + FM_BW(s, 1)) * sc);
    *tx = tn->vis.x;
    *box = (rect){tn->vis.x - 1, n->vis.y, x1 - tn->vis.x + 2, n->vis.h};
    int lh = font_line_height(*f);
    *ty = tn->vis.h > lh ? tn->vis.y + (tn->vis.h - lh) / 2 : tn->vis.y;
    return true;
}

static void draw_text_node(canvas *c, fm_node *t, float op) {
    if (t->app == -3) /* a field being typed into: its editor draws it */
        return;
    fm_node *p = t->parent;
    const fm_style *st = p ? &p->st : &t->st;
    color col = p && p->av.init ? p->av.color : st->color;
    if (t->app == -2) /* placeholder */
        col = color_alpha(col, 0.55f);
    col = color_alpha(col, op);
    if (!C_A(col))
        return;
    float s = t->vscale > 0 ? t->vscale : 1;
    font *f = fmi_text_font(st, s);
    char buf[512];
    fmi_text_for_draw(st, t->text, buf, sizeof buf);
    rect r = t->vis;
    if (st->text_rot) {
        draw_text_turned(c, f, r, buf, guard(c, r, col, st->font_size * s), st, s);
        return;
    }
    int tw = text_width(f, buf), lh = (int)(font_line_height(f) * st->line_height);
    if (fm_text_probe)
        fm_text_probe(c, t, (rect){r.x, r.y, tw < r.w ? tw : r.w, lh}, col, st->font_size * s);
    col = guard(c, (rect){r.x, r.y, tw < r.w ? tw : r.w, lh}, col, st->font_size * s);
    /* Lines aligned as the style says, cut with an ellipsis where the box
     * ends; shadows under them (fm/type.c). */
    if (tw > r.w + 2 && r.h >= lh * 2 - 2 && !st->nowrap) {
        fmi_draw_block(c, f, r, buf, col, st, s);
        return;
    }
    fmi_draw_line_in(c, f, r, buf, col, st, s);
}

/* ---- symbols that aren't apps ---- */

static bool draw_symbol(canvas *c, rect b, const char *name, color col, float sw) {
    float cx = b.x + b.w / 2.0f, cy = b.y + b.h / 2.0f, s = (float)(b.w < b.h ? b.w : b.h), h = s * 0.32f;
    if (!name)
        return false;
    if (!strcasecmp(name, "close") || !strcasecmp(name, "x")) {
        draw_line(c, cx - h, cy - h, cx + h, cy + h, sw, col);
        draw_line(c, cx - h, cy + h, cx + h, cy - h, sw, col);
    } else if (!strcasecmp(name, "minimize") || !strcasecmp(name, "minus")) {
        draw_line(c, cx - h, cy, cx + h, cy, sw, col);
    } else if (!strcasecmp(name, "maximize") || !strcasecmp(name, "square")) {
        stroke_round(c, (rect){(int)(cx - h), (int)(cy - h), (int)(2 * h), (int)(2 * h)}, s * 0.08f, sw, col);
    } else if (!strcasecmp(name, "plus") || !strcasecmp(name, "add")) {
        draw_line(c, cx - h, cy, cx + h, cy, sw, col);
        draw_line(c, cx, cy - h, cx, cy + h, sw, col);
    } else if (!strcasecmp(name, "grid") || !strcasecmp(name, "apps") || !strcasecmp(name, "launcher")) {
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
                fill_circle(c, cx + i * h * 0.85f, cy + j * h * 0.85f, s * 0.07f, col);
    } else if (!strcasecmp(name, "menu") || !strcasecmp(name, "hamburger")) {
        for (int j = -1; j <= 1; j++)
            draw_line(c, cx - h, cy + j * h * 0.7f, cx + h, cy + j * h * 0.7f, sw, col);
    } else if (!strcasecmp(name, "search")) {
        draw_arc(c, cx - s * 0.06f, cy - s * 0.06f, h * 0.75f, 0, 6.2832f, sw, col);
        draw_line(c, cx + h * 0.45f, cy + h * 0.45f, cx + h, cy + h, sw, col);
    } else if (!strcasecmp(name, "dot") || !strcasecmp(name, "circle")) {
        fill_circle(c, cx, cy, h * 0.6f, col);
    } else if (!strcasecmp(name, "ring")) {
        draw_arc(c, cx, cy, h, 0, 6.2832f, sw, col);
    } else if (!strcasecmp(name, "chevron-up") || !strcasecmp(name, "up")) {
        draw_line(c, cx - h, cy + h * 0.4f, cx, cy - h * 0.4f, sw, col);
        draw_line(c, cx, cy - h * 0.4f, cx + h, cy + h * 0.4f, sw, col);
    } else if (!strcasecmp(name, "chevron-down") || !strcasecmp(name, "down")) {
        draw_line(c, cx - h, cy - h * 0.4f, cx, cy + h * 0.4f, sw, col);
        draw_line(c, cx, cy + h * 0.4f, cx + h, cy - h * 0.4f, sw, col);
    } else if (!strcasecmp(name, "chevron-right") || !strcasecmp(name, "right")) {
        draw_line(c, cx - h * 0.4f, cy - h, cx + h * 0.4f, cy, sw, col);
        draw_line(c, cx + h * 0.4f, cy, cx - h * 0.4f, cy + h, sw, col);
    } else if (!strcasecmp(name, "chevron-left") || !strcasecmp(name, "left") || !strcasecmp(name, "back")) {
        draw_line(c, cx + h * 0.4f, cy - h, cx - h * 0.4f, cy, sw, col);
        draw_line(c, cx - h * 0.4f, cy, cx + h * 0.4f, cy + h, sw, col);
    } else if (!strcasecmp(name, "star") || !strcasecmp(name, "spark") || !strcasecmp(name, "sparkle")) {
        /* four long tapered rays and four short ones */
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7854f, len = (i & 1 ? 0.55f : 1.25f) * h;
            for (int k = 0; k < 3; k++) {
                float t0 = k / 3.0f, t1 = (k + 1) / 3.0f;
                draw_line(c, cx + cosf(a) * len * t0, cy + sinf(a) * len * t0, cx + cosf(a) * len * t1,
                          cy + sinf(a) * len * t1, sw * (1.4f - 0.4f * k), col);
            }
        }
        fill_circle(c, cx, cy, sw * 1.5f, col);
    } else if (!strcasecmp(name, "wifi")) {
        for (int i = 1; i <= 3; i++)
            draw_arc(c, cx, cy + h * 0.8f, h * 0.45f * i, -2.36f, -0.78f, sw, col);
        fill_circle(c, cx, cy + h * 0.8f, sw, col);
    } else if (!strcasecmp(name, "bell")) {
        draw_arc(c, cx, cy - h * 0.1f, h * 0.7f, 3.1416f, 6.2832f, sw, col);
        draw_line(c, cx - h * 0.7f, cy - h * 0.1f, cx - h * 0.8f, cy + h * 0.6f, sw, col);
        draw_line(c, cx + h * 0.7f, cy - h * 0.1f, cx + h * 0.8f, cy + h * 0.6f, sw, col);
        draw_line(c, cx - h, cy + h * 0.6f, cx + h, cy + h * 0.6f, sw, col);
        fill_circle(c, cx, cy + h * 0.9f, sw * 1.1f, col);
    } else if (!strcasecmp(name, "home")) {
        draw_line(c, cx - h, cy, cx, cy - h, sw, col);
        draw_line(c, cx, cy - h, cx + h, cy, sw, col);
        stroke_round(c, (rect){(int)(cx - h * 0.7f), (int)cy, (int)(h * 1.4f), (int)(h * 0.9f)}, 1, sw, col);
    } else if (!strcasecmp(name, "moon")) {
        draw_arc(c, cx, cy, h, 0.9f, 5.4f, sw, col);
    } else if (!strcasecmp(name, "sun")) {
        draw_arc(c, cx, cy, h * 0.5f, 0, 6.2832f, sw, col);
        for (int i = 0; i < 8; i++) {
            float a = i * 0.7854f;
            draw_line(c, cx + cosf(a) * h * 0.75f, cy + sinf(a) * h * 0.75f, cx + cosf(a) * h, cy + sinf(a) * h, sw, col);
        }
    } else {
        return false;
    }
    return true;
}

/* Maps symbol names to the shell's app symbols where they exist. */
static int symbol_app(const char *name) {
    if (!name)
        return -1;
    int a = fm_app_index(name);
    if (a >= 0)
        return a;
    static const struct {
        const char *n, *app;
    } alias[] = {{"folder", "files"}, {"files", "files"},  {"chat", "companion"}, {"voice", "companion"},
                 {"ai", "companion"}, {"note", "notes"},   {"edit", "notes"},     {"gear", "settings"},
                 {"cog", "settings"}, {"info", "about"}, {"shell", "terminal"}, {"console", "terminal"}, {"code", "terminal"},   {"palette", "redesign"}, {"brush", "redesign"},
                 {"shutdown", "power"}, {"off", "power"}};
    for (size_t i = 0; i < sizeof alias / sizeof alias[0]; i++)
        if (!strcasecmp(name, alias[i].n))
            return fm_app_index(alias[i].app);
    return -1;
}

static bool is_symbol_name(const char *name);

static void draw_icon_el(canvas *c, fm_node *n, float op) {
    rect r = n->vis;
    float sz = fminf((float)r.w, (float)r.h);
    rect sq = {r.x + (int)((r.w - sz) / 2), r.y + (int)((r.h - sz) / 2), (int)sz, (int)sz};
    bool hover = n->parent && n->parent->hover;
    color col = color_alpha(n->av.init ? n->av.color : n->st.color, op);
    const char *name = fm_attr_get(n, "name");
    /* App icons are drawn by the shell, shaped and tinted by this style. */
    if (n->app >= 0 && !is_symbol_name(name)) {
        fm_style st = n->st;
        st.opacity *= op;
        st.color = col; /* mono icons are drawn in the text colour */
        if (icon_is_line(n->app, st.icon_style, st.icon_shape)) /* iconart.c */
            icon_symbol(c, sq, n->app, fmaxf(1.5f, sz / 14), col);
        else
            fm_host_draw_icon(sq, n->app, &st, hover);
        return;
    }
    if (draw_symbol(c, sq, name, col, fmaxf(1.4f, sz / 14)))
        return;
    int kind = icon_subject_named(name); /* a kind of file: image, audio, ... */
    if (kind >= 0) {
        icon_art(c, sq, kind, icon_look_of(n->st.icon_style), icon_tile_of(n->st.icon_shape), col, n->st.opacity * op,
                 hover);
        return;
    }
    int a = symbol_app(name);
    if (a >= 0) {
        if (icon_is_line(a, n->st.icon_style, n->st.icon_shape))
            icon_symbol(c, sq, a, fmaxf(1.5f, sz / 14), col);
        else /* the app's picture, in the icon style */
            icon_art(c, sq, a, icon_look_of(n->st.icon_style), icon_tile_of(n->st.icon_shape), col,
                     n->st.opacity * op, hover);
        return;
    }
    /* unknown: the first letter in a ring */
    if (name && *name) {
        char one[2] = {(char)(name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0]), 0};
        font *f = fm_host_font(n->st.font, sz * 0.55f);
        draw_text_centered(c, f, sq, one, col);
    }
}

/* draw_symbol(NULL, ...) checks if a name is a symbol without drawing */
static bool is_symbol_name(const char *name) {
    static const char *const names[] = {"close", "x", "minimize", "minus", "maximize", "square", "plus", "add",
                                        "grid", "apps", "launcher", "menu", "hamburger", "search", "dot", "circle",
                                        "ring", "chevron-up", "up", "chevron-down", "down", "chevron-right", "right",
                                        "chevron-left", "left", "back", "star", "spark", "sparkle", "wifi", "bell",
                                        "home", "moon", "sun"};
    for (size_t i = 0; name && i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(name, names[i]))
            return true;
    return false;
}

/* ---- elements ---- */

/* Borders side by side: each its own width and colour, dashed, dotted, double, or carved (inset, outset,
 * ridge, groove: lit from the top left). Square-cornered, as such borders are. */
static void dash_rect(canvas *c, rect band, bool across, int on, int off, color col) {
    int len = across ? band.w : band.h;
    for (int t = 0; t < len; t += on + off) {
        int n = t + on > len ? len - t : on;
        fill_rect(c, across ? (rect){band.x + t, band.y, n, band.h} : (rect){band.x, band.y + t, band.w, n}, col);
    }
}

static void draw_sides(canvas *c, rect r, const fm_style *s, float sc, float op) {
    int w[4];
    color col[4], light[4], dark[4];
    for (int k = 0; k < 4; k++) {
        w[k] = (int)lroundf(FM_BW(s, k) * sc);
        col[k] = s->bsides ? s->bcol[k] : s->border;
        light[k] = color_mix(col[k] | 0xff000000u, 0xffffffffu, 0.55f) & 0x00ffffffu;
        light[k] |= col[k] & 0xff000000u;
        dark[k] = color_mix(col[k] | 0xff000000u, 0xff000000u, 0.5f) & 0x00ffffffu;
        dark[k] |= col[k] & 0xff000000u;
    }
    /* bands: top and left first, so bottom and right take the far corners (as carved edges do) */
    rect band[4] = {{r.x, r.y, r.w, w[0]}, {r.x + r.w - w[1], r.y, w[1], r.h},
                    {r.x, r.y + r.h - w[2], r.w, w[2]}, {r.x, r.y, w[3], r.h}};
    static const int order[4] = {0, 3, 1, 2};
    for (int o = 0; o < 4; o++) {
        int k = order[o];
        if (w[k] <= 0)
            continue;
        bool lit_side = k == 0 || k == 3; /* top and left face the light */
        rect b = band[k];
        bool across = k == 0 || k == 2;
        switch (s->bstyle) {
        case 'd':
            dash_rect(c, b, across, w[k] * 3, w[k] * 2, color_alpha(col[k], op));
            break;
        case 'o':
            dash_rect(c, b, across, w[k], w[k], color_alpha(col[k], op));
            break;
        case '2': { /* two lines, a gap between */
            int t = w[k] / 3 > 0 ? w[k] / 3 : 1;
            rect outer = b, inner = b;
            if (across)
                outer.h = inner.h = t, inner.y = k == 0 ? b.y + w[k] - t : b.y, outer.y = k == 0 ? b.y : b.y + w[k] - t;
            else
                outer.w = inner.w = t, inner.x = k == 3 ? b.x + w[k] - t : b.x, outer.x = k == 3 ? b.x : b.x + w[k] - t;
            fill_rect(c, outer, color_alpha(col[k], op));
            fill_rect(c, inner, color_alpha(col[k], op));
            break;
        }
        case 'i':
        case 'u':
            fill_rect(c, b, color_alpha((s->bstyle == 'u') == lit_side ? light[k] : dark[k], op));
            break;
        case 'r':
        case 'g': { /* the outer half one way, the inner half the other */
            bool out_lit = (s->bstyle == 'r') == lit_side;
            rect outer = b, inner = b;
            int h1 = w[k] / 2 > 0 ? w[k] / 2 : 1;
            if (across) {
                outer.h = h1, inner.h = w[k] - h1;
                if (k == 0) inner.y = b.y + h1; else outer.y = b.y + w[k] - h1;
            } else {
                outer.w = h1, inner.w = w[k] - h1;
                if (k == 3) inner.x = b.x + h1; else outer.x = b.x + w[k] - h1;
            }
            fill_rect(c, outer, color_alpha(out_lit ? light[k] : dark[k], op));
            if (inner.w > 0 && inner.h > 0)
                fill_rect(c, inner, color_alpha(out_lit ? dark[k] : light[k], op));
            break;
        }
        default:
            fill_rect(c, b, color_alpha(col[k], op));
        }
    }
}

static void draw_node(fm_doc *d, canvas *c, fm_node *n, float alpha);
uint64_t prof_now_us(void);
void prof_add(const char *name, uint64_t us);

static void draw_children(fm_doc *d, canvas *c, fm_node *n, float op) {
    if (n->rich_on && n->rich) { /* a paragraph: its words, whoever they belong to */
        fmi_rich_draw(c, n, op);
        return;
    }
    fm_node *kids[256];
    int nk = 0;
    for (fm_node *k = n->first; k && nk < 256; k = k->next) {
        if (k->kind == FM_TEXT) {
            if (in_flow(k) && k->laid_out)
                kids[nk++] = k;
        } else if (!is_popover(k) && !is_meta(k) && !k->st.hidden && k->laid_out && !tag_is(k, "tip")) {
            kids[nk++] = k;
        }
    }
    /* z-index among siblings (stable) */
    for (int i = 1; i < nk; i++) {
        fm_node *x = kids[i];
        int j = i - 1;
        while (j >= 0 && kids[j]->kind == FM_ELEM && x->kind == FM_ELEM && kids[j]->st.z > x->st.z) {
            kids[j + 1] = kids[j];
            j--;
        }
        kids[j + 1] = x;
    }
    for (int i = 0; i < nk; i++) {
        if (kids[i]->kind == FM_TEXT)
            { uint64_t pt0 = prof_now_us(); draw_text_node(c, kids[i], op); prof_add("text", prof_now_us() - pt0); }
        else
            draw_node(d, c, kids[i], op);
    }
    /* tooltips of hovered elements */
    for (fm_node *k = n->first; k; k = k->next)
        if (tag_is(k, "tip") && n->a_hover > 0.05f && k->laid_out)
            draw_node(d, c, k, op * n->a_hover);
}

double fm_pacc[8]; /* PROFILE */
static double pnow(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
/* ---- layers: an element drawn on its own, then laid down turned, filtered, clipped, masked or blended ----
 * The canvas blends as if what is under is opaque, so the element is drawn twice, over black and over
 * white: where the two differ is how see-through it is, and what it is made of. */

#define LAYER_DEPTH 3
static canvas *layer_black[LAYER_DEPTH], *layer_white[LAYER_DEPTH];
static int layer_depth;
static fm_node *layer_root[LAYER_DEPTH];

static bool needs_layer(const fm_node *n) {
    const fm_style *s = &n->st;
    return fabsf(s->rotate) > 0.05f || fabsf(s->skew_x) > 0.05f || fabsf(s->skew_y) > 0.05f || s->has_filter ||
           s->blend || s->clip || s->mask.kind != FM_BG_NONE;
}

static canvas *layer_canvas(canvas **slot, const canvas *like) {
    if (*slot && ((*slot)->w != like->w || (*slot)->h != like->h)) {
        canvas_free(*slot);
        *slot = NULL;
    }
    if (!*slot)
        *slot = canvas_new(like->w, like->h);
    return *slot;
}

static float lum(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

static void filter_px(const fm_style *s, float *r, float *g, float *b, float *a) {
    if (s->f_bright != 1) *r *= s->f_bright, *g *= s->f_bright, *b *= s->f_bright;
    if (s->f_contrast != 1) {
        *r = (*r - 0.5f) * s->f_contrast + 0.5f, *g = (*g - 0.5f) * s->f_contrast + 0.5f;
        *b = (*b - 0.5f) * s->f_contrast + 0.5f;
    }
    if (s->f_gray > 0) {
        float l = lum(*r, *g, *b), k = fminf(s->f_gray, 1);
        *r += (l - *r) * k, *g += (l - *g) * k, *b += (l - *b) * k;
    }
    if (s->f_sepia > 0) {
        float k = fminf(s->f_sepia, 1);
        float sr = *r * 0.393f + *g * 0.769f + *b * 0.189f, sg = *r * 0.349f + *g * 0.686f + *b * 0.168f,
              sb = *r * 0.272f + *g * 0.534f + *b * 0.131f;
        *r += (sr - *r) * k, *g += (sg - *g) * k, *b += (sb - *b) * k;
    }
    if (s->f_sat != 1) {
        float l = lum(*r, *g, *b);
        *r = l + (*r - l) * s->f_sat, *g = l + (*g - l) * s->f_sat, *b = l + (*b - l) * s->f_sat;
    }
    if (s->f_hue != 0) { /* rotation about the grey axis (CSS's matrix) */
        float a = s->f_hue * (float)M_PI / 180, cs = cosf(a), sn = sinf(a);
        float R = *r, G = *g, B = *b;
        *r = R * (0.213f + cs * 0.787f - sn * 0.213f) + G * (0.715f - cs * 0.715f - sn * 0.715f) +
             B * (0.072f - cs * 0.072f + sn * 0.928f);
        *g = R * (0.213f - cs * 0.213f + sn * 0.143f) + G * (0.715f + cs * 0.285f + sn * 0.140f) +
             B * (0.072f - cs * 0.072f - sn * 0.283f);
        *b = R * (0.213f - cs * 0.213f - sn * 0.787f) + G * (0.715f - cs * 0.715f + sn * 0.715f) +
             B * (0.072f + cs * 0.928f + sn * 0.072f);
    }
    if (s->f_invert > 0) {
        float k = fminf(s->f_invert, 1);
        *r += (1 - 2 * *r) * k, *g += (1 - 2 * *g) * k, *b += (1 - 2 * *b) * k;
    }
    *a *= s->f_opacity;
}

static float blend_ch(char m, float b, float s) { /* backdrop b, source s, 0..1 */
    switch (m) {
    case 'm': return b * s;
    case 's': return b + s - b * s;
    case 'o': return b < 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 'h': return s < 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    case 'd': return fminf(b, s);
    case 'l': return fmaxf(b, s);
    case 'D': return fabsf(b - s);
    case 'x': return b + s - 2 * b * s;
    case 'c': return s >= 1 ? 1 : fminf(1, b / (1 - s));
    case 'b': return s <= 0 ? 0 : 1 - fminf(1, (1 - b) / s);
    case 'S': return s <= 0.5f ? b - (1 - 2 * s) * b * (1 - b) : b + (2 * s - 1) * ((b <= 0.25f ? ((16 * b - 12) * b + 4) * b : sqrtf(b)) - b);
    default: return s;
    }
}

/* how much of point (px, py) of box b the clip path lets through, 0..1 (soft by a pixel) */
static float clip_cover(const fm_style *s, rect b, float px, float py) {
    float W = (float)b.w, H = (float)b.h, x = px - b.x, y = py - b.y;
    switch (s->clip) {
    case 'c':
    case 'e': {
        float rx = s->clip_v[0] * (s->clip == 'c' ? sqrtf(W * W + H * H) / 1.41421f : W);
        float ry = s->clip == 'c' ? rx : s->clip_v[1] * H;
        float dx = (x - s->clip_v[2] * W) / (rx > 0 ? rx : 1), dy = (y - s->clip_v[3] * H) / (ry > 0 ? ry : 1);
        float d = (sqrtf(dx * dx + dy * dy) - 1) * fminf(rx, ry);
        return d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;
    }
    case 'i': {
        float t[4];
        for (int k = 0; k < 4; k++)
            t[k] = s->clip_v[k] < 0 ? -s->clip_v[k] : s->clip_v[k] * ((k % 2) ? W : H);
        float x0 = t[3], y0 = t[0], x1 = W - t[1], y1 = H - t[2], rr = s->clip_v[4];
        float qx = fmaxf(fmaxf(x0 + rr - x, x - (x1 - rr)), 0), qy = fmaxf(fmaxf(y0 + rr - y, y - (y1 - rr)), 0);
        float d = sqrtf(qx * qx + qy * qy) - rr;
        if (x < x0 || x > x1 || y < y0 || y > y1)
            d = fmaxf(d, 0.5f);
        return d <= -0.5f ? 1 : d >= 0.5f ? 0 : 0.5f - d;
    }
    case 'p': { /* even-odd, a point sampled at the pixel centre */
        int n = s->clip_n / 2;
        bool in = false;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float xi = s->clip_v[2 * i] < 0 ? -s->clip_v[2 * i] : s->clip_v[2 * i] * W;
            float yi = s->clip_v[2 * i + 1] < 0 ? -s->clip_v[2 * i + 1] : s->clip_v[2 * i + 1] * H;
            float xj = s->clip_v[2 * j] < 0 ? -s->clip_v[2 * j] : s->clip_v[2 * j] * W;
            float yj = s->clip_v[2 * j + 1] < 0 ? -s->clip_v[2 * j + 1] : s->clip_v[2 * j + 1] * H;
            if (((yi > y) != (yj > y)) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
                in = !in;
        }
        return in ? 1 : 0;
    }
    }
    return 1;
}

/* one box blur of region R of a straight-alpha layer, rows then columns, by running sums */
static void blur_layer(canvas *L, rect R, int rad) {
    if (rad < 1)
        return;
    static float buf[4][8192];
    for (int dir = 0; dir < 2; dir++) {
        int lines = dir ? R.w : R.h, len = dir ? R.h : R.w;
        if (len > 8192)
            return;
        for (int l = 0; l < lines; l++) {
            for (int t = 0; t < len; t++) {
                int x = dir ? R.x + l : R.x + t, y = dir ? R.y + t : R.y + l;
                uint32_t p = L->px[(size_t)y * (size_t)L->w + (size_t)x];
                float a = C_A(p) / 255.0f;
                buf[0][t] = a, buf[1][t] = C_R(p) * a, buf[2][t] = C_G(p) * a, buf[3][t] = C_B(p) * a;
            }
            float sum[4] = {0};
            int n = 0;
            for (int t = 0; t < rad && t < len; t++, n++)
                for (int ch = 0; ch < 4; ch++)
                    sum[ch] += buf[ch][t];
            for (int t = 0; t < len; t++) {
                int add = t + rad, sub = t - rad - 1;
                if (add < len) {
                    for (int ch = 0; ch < 4; ch++)
                        sum[ch] += buf[ch][add];
                    n++;
                }
                if (sub >= 0) {
                    for (int ch = 0; ch < 4; ch++)
                        sum[ch] -= buf[ch][sub];
                    n--;
                }
                float a = sum[0] / n;
                int x = dir ? R.x + l : R.x + t, y = dir ? R.y + t : R.y + l;
                L->px[(size_t)y * (size_t)L->w + (size_t)x] =
                    a > 0.002f ? (uint32_t)(fminf(1, a) * 255) << 24 | (uint32_t)fminf(255, sum[1] / n / a) << 16 |
                                     (uint32_t)fminf(255, sum[2] / n / a) << 8 | (uint32_t)fminf(255, sum[3] / n / a)
                               : 0;
            }
        }
    }
}

static color shade_grad(void *ctx, int x, int y);
static void draw_node_now(fm_doc *d, canvas *c, fm_node *n, float alpha);

static void draw_layered(fm_doc *d, canvas *c, fm_node *n, float alpha) {
    const fm_style *s = &n->st;
    int depth = layer_depth;
    canvas *B = layer_canvas(&layer_black[depth], c), *W = layer_canvas(&layer_white[depth], c);
    if (!B || !W) {
        draw_node_now(d, c, n, alpha);
        return;
    }
    /* the region it may cover: its box, its shadows and glow, room to turn */
    rect v = n->vis;
    float sc = n->vscale > 0 ? n->vscale : 1;
    int m = (int)(fmaxf(s->shadow_blur + fmaxf(fabsf(s->shadow_x), fabsf(s->shadow_y)), s->glow_size) * sc) + 4;
    for (int i = 0; i < s->nshadow; i++)
        m = (int)fmaxf((float)m, (s->sh_blur[i] + fmaxf(fabsf(s->sh_x[i]), fabsf(s->sh_y[i])) + s->sh_spread[i]) * sc + 4);
    rect R = {v.x - m, v.y - m, v.w + 2 * m, v.h + 2 * m};
    if (R.x < 0) R.w += R.x, R.x = 0;
    if (R.y < 0) R.h += R.y, R.y = 0;
    if (R.x + R.w > c->w) R.w = c->w - R.x;
    if (R.y + R.h > c->h) R.h = c->h - R.y;
    if (R.w <= 0 || R.h <= 0)
        return;
    canvas *both[2] = {B, W};
    for (int k = 0; k < 2; k++) {
        canvas *L = both[k];
        L->cx0 = R.x, L->cy0 = R.y, L->cx1 = R.x + R.w, L->cy1 = R.y + R.h;
        for (int y = R.y; y < R.y + R.h; y++)
            for (int x = R.x; x < R.x + R.w; x++)
                L->px[(size_t)y * (size_t)L->w + (size_t)x] = k ? 0xffffffffu : 0xff000000u;
        layer_root[depth] = n;
        layer_depth++;
        draw_node_now(d, L, n, alpha);
        layer_depth--;
    }
    /* what it is: colour and how see-through, straight (not premultiplied), filtered */
    for (int y = R.y; y < R.y + R.h; y++)
        for (int x = R.x; x < R.x + R.w; x++) {
            size_t i = (size_t)y * (size_t)c->w + (size_t)x;
            uint32_t kb = B->px[i], kw = W->px[i];
            float a = 1 - ((float)(C_R(kw) - C_R(kb)) + (float)(C_G(kw) - C_G(kb)) + (float)(C_B(kw) - C_B(kb))) / (3 * 255.0f);
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            float r = 0, g = 0, b = 0;
            if (a > 0.002f)
                r = C_R(kb) / 255.0f / a, g = C_G(kb) / 255.0f / a, b = C_B(kb) / 255.0f / a;
            if (s->has_filter)
                filter_px(s, &r, &g, &b, &a);
            if (s->clip)
                a *= clip_cover(s, n->vis, x + 0.5f, y + 0.5f);
            if (s->mask.kind != FM_BG_NONE) { /* the mask's opacity, laid over its box */
                struct grad gm = {s->mask, n->vis, 1, 0, 0, 1};
                float ang = (s->mask.angle - 90) * (float)M_PI / 180;
                gm.dx = cosf(ang), gm.dy = sinf(ang);
                gm.len = fmaxf(1, fabsf(n->vis.w * gm.dx) + fabsf(n->vis.h * gm.dy));
                a *= C_A(shade_grad(&gm, x, y)) / 255.0f;
            }
            r = r < 0 ? 0 : r > 1 ? 1 : r, g = g < 0 ? 0 : g > 1 ? 1 : g, b = b < 0 ? 0 : b > 1 ? 1 : b;
            B->px[i] = (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)(r * 255 + 0.5f) << 16 |
                       (uint32_t)(g * 255 + 0.5f) << 8 | (uint32_t)(b * 255 + 0.5f);
        }
    if (s->f_blur > 0.3f) /* blur what it is (premultiplied, so edges fade rather than darken) */
        for (int pass = 0; pass < 3; pass++) /* three box blurs: close to a Gaussian */
            blur_layer(B, R, (int)(s->f_blur * sc * 0.6f + 0.5f));
    /* laid down: turned and skewed about its centre (each screen pixel looks back into the layer) */
    float cx = n->vis.x + n->vis.w / 2.0f, cy = n->vis.y + n->vis.h / 2.0f;
    float a = s->rotate * (float)M_PI / 180, ca = cosf(a), sa = sinf(a);
    float kx = tanf(s->skew_x * (float)M_PI / 180), ky = tanf(s->skew_y * (float)M_PI / 180);
    /* forward: p' = c + Rot * Skew * (p - c); Skew = [1 kx; ky 1] */
    float m00 = ca * 1 - sa * ky, m01 = ca * kx - sa * 1, m10 = sa * 1 + ca * ky, m11 = sa * kx + ca * 1;
    float det = m00 * m11 - m01 * m10;
    if (fabsf(det) < 1e-4f)
        return;
    float i00 = m11 / det, i01 = -m01 / det, i10 = -m10 / det, i11 = m00 / det;
    bool turned = fabsf(s->rotate) > 0.05f || fabsf(s->skew_x) > 0.05f || fabsf(s->skew_y) > 0.05f;
    /* where it lands: the corners of R, moved */
    float xs[4] = {(float)R.x, (float)(R.x + R.w), (float)R.x, (float)(R.x + R.w)};
    float ys[4] = {(float)R.y, (float)R.y, (float)(R.y + R.h), (float)(R.y + R.h)};
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int k = 0; k < 4; k++) {
        float dx = xs[k] - cx, dy = ys[k] - cy;
        float X = turned ? cx + m00 * dx + m01 * dy : xs[k], Y = turned ? cy + m10 * dx + m11 * dy : ys[k];
        minx = fminf(minx, X), maxx = fmaxf(maxx, X), miny = fminf(miny, Y), maxy = fmaxf(maxy, Y);
    }
    int X0 = (int)floorf(minx), Y0 = (int)floorf(miny), X1 = (int)ceilf(maxx), Y1 = (int)ceilf(maxy);
    if (X0 < c->cx0) X0 = c->cx0;
    if (Y0 < c->cy0) Y0 = c->cy0;
    if (X1 > c->cx1) X1 = c->cx1;
    if (Y1 > c->cy1) Y1 = c->cy1;
    for (int y = Y0; y < Y1; y++)
        for (int x = X0; x < X1; x++) {
            float sx = x + 0.5f, sy = y + 0.5f;
            if (turned) {
                float dx = sx - cx, dy = sy - cy;
                sx = cx + i00 * dx + i01 * dy, sy = cy + i10 * dx + i11 * dy;
            }
            /* bilinear, premultiplied */
            float fx = sx - 0.5f, fy = sy - 0.5f;
            int ix = (int)floorf(fx), iy = (int)floorf(fy);
            float tx = fx - ix, ty = fy - iy;
            float acc[4] = {0};
            for (int q = 0; q < 4; q++) {
                int qx = ix + (q & 1), qy = iy + (q >> 1);
                float w = ((q & 1) ? tx : 1 - tx) * ((q >> 1) ? ty : 1 - ty);
                if (qx < R.x || qy < R.y || qx >= R.x + R.w || qy >= R.y + R.h || w <= 0)
                    continue;
                uint32_t p = B->px[(size_t)qy * (size_t)c->w + (size_t)qx];
                float pa = C_A(p) / 255.0f * w;
                acc[0] += pa, acc[1] += C_R(p) * pa, acc[2] += C_G(p) * pa, acc[3] += C_B(p) * pa;
            }
            if (acc[0] < 0.002f)
                continue;
            float sa_ = acc[0];
            float sr = acc[1] / sa_ / 255, sg = acc[2] / sa_ / 255, sb = acc[3] / sa_ / 255;
            uint32_t *dp = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
            float br = C_R(*dp) / 255.0f, bg = C_G(*dp) / 255.0f, bb = C_B(*dp) / 255.0f;
            if (s->blend)
                sr = blend_ch(s->blend, br, sr), sg = blend_ch(s->blend, bg, sg), sb = blend_ch(s->blend, bb, sb);
            float k = sa_ > 1 ? 1 : sa_;
            float r = br + (sr - br) * k, g = bg + (sg - bg) * k, b = bb + (sb - bb) * k;
            *dp = 0xff000000u | (uint32_t)(fminf(1, fmaxf(0, r)) * 255 + 0.5f) << 16 |
                  (uint32_t)(fminf(1, fmaxf(0, g)) * 255 + 0.5f) << 8 | (uint32_t)(fminf(1, fmaxf(0, b)) * 255 + 0.5f);
        }
}

static void draw_node(fm_doc *d, canvas *c, fm_node *n, float alpha) {
    if (n->kind == FM_ELEM && !n->st.hidden && n->laid_out && needs_layer(n) && layer_depth < LAYER_DEPTH &&
        (layer_depth == 0 || layer_root[layer_depth - 1] != n)) {
        draw_layered(d, c, n, alpha);
        return;
    }
    draw_node_now(d, c, n, alpha);
}

static fm_node *frame_timed; /* an app's window frame, or a desktop surface, being drawn: its parts in the log */
static const char *const frame_parts[4] = {"frame shadow", "frame glass", "frame background", "frame contents"};
static const char *const surface_parts[4] = {"surface shadow", "surface glass", "surface background", "surface contents"};
static const char *const *timed_parts = frame_parts;
static void draw_node_now(fm_doc *d, canvas *c, fm_node *n, float alpha) {
    const fm_style *s = &n->st;
    double t0 = pnow();
    bool timed = n == frame_timed;
    if (s->hidden || !n->laid_out)
        return;
    bool anim = n->av.init;
    float op = alpha * (anim ? n->av.opacity : s->opacity);
    if (op < 0.01f)
        return;
    rect r = n->vis;
    if (r.w <= 0 || r.h <= 0)
        return;
    float sc = n->vscale > 0 ? n->vscale : 1;
    float rad[4] = {s->radius[0] * sc, s->radius[1] * sc, s->radius[2] * sc, s->radius[3] * sc};
    /* A child sitting in a rounded corner of something around it (a title
     * bar in a window frame) is rounded there too; otherwise its square
     * corner pokes out past the curve, since clipping is rectangular. */
    for (const fm_node *p = n->parent; p && p->kind == FM_ELEM; p = p->parent) {
        rect pr = p->vis;
        float ps = p->vscale > 0 ? p->vscale : 1, bw = p->st.border_w * ps, slack = bw + 1.5f;
        const int cx[4] = {0, 1, 1, 0}, cy[4] = {0, 0, 1, 1}; /* tl tr br bl */
        for (int k = 0; k < 4; k++) {
            float pr_k = p->st.radius[k] * ps - bw;
            if (pr_k <= rad[k])
                continue;
            bool x_on = cx[k] ? fabsf((float)(r.x + r.w) - (float)(pr.x + pr.w)) <= slack : fabsf((float)(r.x - pr.x)) <= slack;
            bool y_on = cy[k] ? fabsf((float)(r.y + r.h) - (float)(pr.y + pr.h)) <= slack : fabsf((float)(r.y - pr.y)) <= slack;
            if (x_on && y_on)
                rad[k] = pr_k;
        }
    }
    float r0 = corner_r(rad, r);
    fm_bg bg = anim ? n->av.bg : s->bg;

    /* shadow and glow sit outside the box */
    color sh = anim ? n->av.shadow : s->shadow;
    if (s->nshadow) { /* the last listed first, so the first sits on top */
        for (int i = s->nshadow - 1; i >= 0; i--) {
            color col = color_alpha(i == 0 ? sh : s->sh_c[i], op);
            if (!C_A(col))
                continue;
            float sp = s->sh_spread[i] * sc;
            rect sr = {r.x + (int)lroundf(s->sh_x[i] * sc - sp), r.y + (int)lroundf(s->sh_y[i] * sc - sp),
                       r.w + (int)lroundf(2 * sp), r.h + (int)lroundf(2 * sp)};
            float rr = r0 + sp > 0 ? r0 + sp : 0;
            if (s->sh_blur[i] < 0.5f)
                fill_round(c, sr, rr, col); /* a hard shadow, offset */
            else
                shadow_round(c, sr, rr, s->sh_blur[i] * sc, col);
        }
    } else if (s->shadow_blur > 0 && C_A(sh)) {
        /* an opaque box covers its own middle: its shadow there would be painted for nothing */
        bool opaque = bg.kind == FM_BG_SOLID && C_A(bg.a) == 255 && op >= 0.999f && s->blur <= 0.5f && !s->bg_img[0];
        int in = (int)ceilf(r0) + 1;
        rect hole = opaque && r.w > 2 * in && r.h > 2 * in ? (rect){r.x + in, r.y + in, r.w - 2 * in, r.h - 2 * in}
                                                           : (rect){0, 0, 0, 0};
        shadow_round_hollow(c, (rect){r.x + (int)(s->shadow_x * sc), r.y + (int)(s->shadow_y * sc), r.w, r.h}, r0,
                            s->shadow_blur * sc, color_alpha(sh, op), hole);
    }
    double t1 = pnow(); fm_pacc[0] += t1 - t0;
    float gs = anim ? n->av.glow_size : s->glow_size;
    if (gs > 0.5f)
        glow_round(c, r, r0, gs * sc, color_alpha(anim ? n->av.glow : s->glow, op));

    /* glass: blur and lift what's behind; not when its own background covers it all, opaque (a design's solid
     * frame over the built-in "blur: 18": a blur nobody could see, a fifth of a core with a big window open) */
    double t2 = pnow(); fm_pacc[1] += t2 - t1;
    bool covers = bg.kind == FM_BG_SOLID && C_A(bg.a) == 255 && op >= 0.999f && !s->bg_img[0];
    if (s->blur > 0.5f && op > 0.3f && !covers)
        blur_fast(c, r, r0, (int)fminf(s->blur, 40));
    double t3 = pnow(); fm_pacc[2] += t3 - t2;
    if (s->saturate > 1.02f && op > 0.3f && !covers)
        vibrancy(c, r, r0, s->saturate, 0.02f);
    double t4 = pnow(); fm_pacc[3] += t4 - t3;
    if (s->bg_img[0] && !s->bg_img_over) { /* the colour under the picture, then the picture */
        paint_bg(c, r, rad, bg, op);
        paint_pic(c, r, rad, s, op);
    } else if (s->bg_img[0]) { /* the picture, a gradient over it */
        paint_pic(c, r, rad, s, op);
        paint_bg(c, r, rad, bg, op);
    } else if (blur_hole.w > 0 && blur_hole.x >= r.x && blur_hole.y >= r.y && blur_hole.x + blur_hole.w <= r.x + r.w &&
               blur_hole.y + blur_hole.h <= r.y + r.h) {
        /* a window's frame, its app painted over its middle next: the background only around that (the same
         * paint, clipped to the four sides: a gradient stays one gradient), not 1.4 million pixels for nothing */
        rect h = blur_hole, side[4] = {{r.x, r.y, r.w, h.y - r.y},
                                       {r.x, h.y + h.h, r.w, r.y + r.h - (h.y + h.h)},
                                       {r.x, h.y, h.x - r.x, h.h},
                                       {h.x + h.w, h.y, r.x + r.w - (h.x + h.w), h.h}};
        int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
        for (int k = 0; k < 4; k++) {
            rect b = side[k];
            int x0 = b.x > cx0 ? b.x : cx0, y0 = b.y > cy0 ? b.y : cy0;
            int x1 = b.x + b.w < cx1 ? b.x + b.w : cx1, y1 = b.y + b.h < cy1 ? b.y + b.h : cy1;
            if (x1 <= x0 || y1 <= y0)
                continue;
            c->cx0 = x0, c->cy0 = y0, c->cx1 = x1, c->cy1 = y1;
            paint_bg(c, r, rad, bg, op);
        }
        c->cx0 = cx0, c->cy0 = cy0, c->cx1 = cx1, c->cy1 = cy1;
    } else {
        paint_bg(c, r, rad, bg, op);
    }
    double t5 = pnow(); fm_pacc[4] += t5 - t4;
    if (timed) {
        prof_add(timed_parts[0], (uint64_t)((t1 - t0) * 1e3));
        prof_add(timed_parts[1], (uint64_t)((t3 - t2) * 1e3));
        prof_add(timed_parts[2], (uint64_t)((t5 - t4) * 1e3));
    }
    /* glass catches light along its top edge */
    if (s->blur > 0.5f && op > 0.3f && r.h > 12)
        stroke_round_vgradient(c, rect_inset(r, 1), r0 > 1 ? r0 - 1 : 0, 1, RGBA(255, 255, 255, (int)(46 * op)),
                               RGBA(255, 255, 255, 0));

    /* inset shadows without blur: edges inside the border, the first on top (how 98.css draws bevels) */
    if (s->ninset) {
        rect in = {r.x + (int)(FM_BW(s, 3) * sc), r.y + (int)(FM_BW(s, 0) * sc),
                   r.w - (int)((FM_BW(s, 1) + FM_BW(s, 3)) * sc), r.h - (int)((FM_BW(s, 0) + FM_BW(s, 2)) * sc)};
        for (int i = s->ninset - 1; i >= 0; i--) {
            int dx = (int)lroundf(s->inset_x[i] * sc), dy = (int)lroundf(s->inset_y[i] * sc);
            color col = color_alpha(s->inset_c[i], op);
            if (dy > 0)
                fill_rect(c, (rect){in.x, in.y, in.w, dy}, col);
            else if (dy < 0)
                fill_rect(c, (rect){in.x, in.y + in.h + dy, in.w, -dy}, col);
            if (dx > 0)
                fill_rect(c, (rect){in.x, in.y, dx, in.h}, col);
            else if (dx < 0)
                fill_rect(c, (rect){in.x + in.w + dx, in.y, -dx, in.h}, col);
        }
    }

    if (s->border_w > 0 && (s->bsides || s->bstyle)) {
        draw_sides(c, r, s, sc, op);
    } else if (s->border_w > 0) {
        float bw = s->border_w * sc;
        if (s->border_bg.kind == FM_BG_LINEAR)
            stroke_round_vgradient(c, r, r0, bw, color_alpha(s->border_bg.a, op), color_alpha(s->border_bg.b, op));
        else
            stroke_round(c, r, r0, bw, color_alpha(anim ? n->av.border : s->border, op));
    }

    /* content */
    if (tag_is(n, "icon")) {
        { uint64_t pt0 = prof_now_us(); draw_icon_el(c, n, op); prof_add("icons", prof_now_us() - pt0); }
    } else if (tag_is(n, "orb")) {
        fm_host_draw_orb(r, n->hover || n->open);
    } else if (tag_is(n, "avatar")) {
        fm_host_draw_avatar(r, fm_attr_get(n, "frame"), fm_attr_get(n, "tint"), fm_attr_get(n, "shot"),
                            n->hover || n->open);
    } else if (tag_is(n, "view")) {
        const char *app = fm_attr_get(n, "app");
        clipbox cb = clip_push(c, r);
        fm_host_draw_view(app ? app : "companion", r);
        clip_pop(c, cb);
    } else if (tag_is(n, "clock")) {
        const char *face = fm_attr_get(n, "face");
        if (face && (!strcasecmp(face, "ring") || !strcasecmp(face, "arc"))) {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, R = fminf(r.w, r.h) / 2.0f - 3;
            color col = anim ? n->av.color : s->color;
            draw_arc(c, cx, cy, R, 0, 6.2832f, 2 * sc, color_alpha(col, op * 0.18f));
            float frac = (tm.tm_min * 60 + tm.tm_sec) / 3600.0f;
            draw_arc(c, cx, cy, R, -1.5708f, -1.5708f + frac * 6.2832f, 3 * sc, color_alpha(col, op * 0.9f));
            float ha = -1.5708f + ((tm.tm_hour % 12) + tm.tm_min / 60.0f) / 12 * 6.2832f;
            fill_circle(c, cx + cosf(ha) * R, cy + sinf(ha) * R, 4 * sc, color_alpha(col, op));
        }
    } else if (is_tray(n)) {
        fm_host_draw_tray(n, r, anim ? n->av.color : s->color, op);
    } else if (tag_is(n, "canvas")) {
        clipbox cc = clip_push(c, r);
        fmi_canvas_draw(d, c, n, r, op);
        clip_pop(c, cc);
    } else if (tag_is(n, "model")) {
        float pl = s->pad[3] + FM_BW(s, 3), pt = s->pad[0] + FM_BW(s, 0);
        rect in = {r.x + (int)(pl * sc), r.y + (int)(pt * sc), r.w - (int)((pl + s->pad[1] + FM_BW(s, 1)) * sc),
                   r.h - (int)((pt + s->pad[2] + FM_BW(s, 2)) * sc)};
        clipbox cc = clip_push(c, r);
        fmi_model_draw(d, c, n, in, op);
        clip_pop(c, cc);
    } else if (tag_is(n, "image") || tag_is(n, "svg") || tag_is(n, "pixels")) {
        float pl = s->pad[3] + FM_BW(s, 3), pt = s->pad[0] + FM_BW(s, 0);
        rect in = {r.x + (int)(pl * sc), r.y + (int)(pt * sc), r.w - (int)((pl + s->pad[1] + FM_BW(s, 1)) * sc),
                   r.h - (int)((pt + s->pad[2] + FM_BW(s, 2)) * sc)};
        clipbox cc = clip_push(c, r);
        fmi_media_draw(c, n, in, op);
        clip_pop(c, cc);
    } else if (tag_is(n, "divider")) {
        if (bg.kind == FM_BG_NONE)
            fill_rect(c, r, color_alpha(RGBA(255, 255, 255, 50), op));
    }

    bool clip = s->overflow != FM_OV_VISIBLE;
    clipbox cb = {0};
    if (clip)
        cb = clip_push(c, r);
    bool editing = d->focus == n && (tag_is(n, "search") || tag_is(n, "input"));
    if (editing) /* its value is drawn by the editor below, with the caret and selection */
        for (fm_node *k = n->first; k; k = k->next)
            if (k->kind == FM_TEXT && k->generated && k->app == -1)
                k->app = -3;
    uint64_t ct0 = timed ? prof_now_us() : 0;
    draw_children(d, c, n, op);
    if (timed)
        prof_add(timed_parts[3], prof_now_us() - ct0);
    if (editing)
        for (fm_node *k = n->first; k; k = k->next)
            if (k->app == -3)
                k->app = -1;
    if (clip)
        clip_pop(c, cb);
    /* a scrollbar, when there is more than fits (scrollbar-color / scrollbar-width) */
    if (s->overflow == FM_OV_SCROLL && s->sb_width >= 0) {
        float pt = s->pad[0] + FM_BW(s, 0), pb = s->pad[2] + FM_BW(s, 2);
        int view = r.h - (int)((pt + pb) * sc);
        if (n->content_h > view + 1 && view > 20) {
            float w = (s->sb_width > 0 ? s->sb_width : 8) * sc;
            color thumb = s->sb_thumb ? s->sb_thumb : color_alpha(s->color, 0.35f), track = s->sb_track;
            rect tr = {r.x + r.w - (int)w - (int)(FM_BW(s, 1) * sc) - 2, r.y + (int)(pt * sc), (int)w, view};
            if (C_A(track))
                fill_round(c, tr, s->sb_width >= 10 ? 0 : w / 2, color_alpha(track, op));
            int th = (int)fmaxf(24, (float)view * view / n->content_h);
            int ty = tr.y + (int)((float)(view - th) * n->scroll / fmaxf(1, (float)(n->content_h - view)));
            fill_round(c, (rect){tr.x, ty, tr.w, th}, s->sb_width >= 10 ? 0 : w / 2, color_alpha(thumb, op));
        }
    }

    /* the text caret of a focused field */
    if (editing) {
        font *ef;
        rect box;
        int tx, ty;
        if (fmi_input_geom(n, &ef, &box, &tx, &ty)) {
            const char *v = fm_attr_get(n, "value");
            const char *type = fm_attr_get(n, "type");
            color col = color_alpha(anim ? n->av.color : s->color, op);
            clipbox ec = clip_push(c, r);
            te_draw(c, fmi_editor(n), v ? v : "", ef, box, tx, ty, col, color_alpha(col, 0.25f), true,
                    type && !strcasecmp(type, "password"), fmi_now_ms());
            clip_pop(c, ec);
        }
    }

    /* the companion just touched this */
    if (n->flash > 0.01f) {
        float f = n->flash;
        glow_round(c, r, r0, 18 * f + 4, RGBA(255, 255, 255, (int)(150 * f)));
        stroke_round(c, rect_inset(r, -3), r0 + 3, 2, RGBA(255, 255, 255, (int)(220 * f)));
    }
}

/* ---- layers ---- */

int fm_layer_of(const fm_node *n) {
    if (is_popover(n))
        return 2;
    const char *l = fm_attr_get(n, "layer");
    if (l) {
        if (!strcasecmp(l, "below") || !strcasecmp(l, "desktop") || !strcmp(l, "0") || !strcasecmp(l, "back"))
            return 0;
        return 1;
    }
    if (tag_is(n, "taskbar") || tag_is(n, "dock") || tag_is(n, "menubar") || tag_is(n, "bar"))
        return 1;
    return 0;
}

static void draw_popovers(fm_doc *d, canvas *c, fm_node *n) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM || is_meta(n))
            continue;
        if (is_popover(n) && n->a_open > 0.01f && n->laid_out && !n->st.hidden) {
            fmi_transform(n, 1, 0, 0);
            draw_node(d, c, n, n->a_open);
        }
        draw_popovers(d, c, n->first);
    }
}

uint64_t prof_now_us(void);
void prof_add(const char *name, uint64_t us);
fm_doc *fm_host_desktop_doc(void);
uint64_t fm_host_backdrop_sig(void); /* what lies under the desktop's surfaces, as a number (0: nothing to keep by) */

void fm_render_layer(fm_doc *d, canvas *c, int layer) {
    if (!d->desktop)
        return;
    if (layer == 2) {
        draw_popovers(d, c, d->desktop->first);
        return;
    }
    /* A background on <desktop> itself tints (or covers) the wallpaper. */
    const fm_style *ds = &d->desktop->st;
    if (layer == 0 && ds->bg.kind != FM_BG_NONE) {
        float none[4] = {0, 0, 0, 0};
        paint_bg(c, d->screen, none, d->desktop->av.init ? d->desktop->av.bg : ds->bg,
                 d->desktop->av.init ? d->desktop->av.opacity : ds->opacity);
    }
    fm_node *surf[128];
    int ns = 0;
    for (fm_node *k = d->desktop->first; k && ns < 128; k = k->next)
        if (k->kind == FM_ELEM && !is_meta(k) && !is_popover(k) && !k->st.hidden && k->laid_out &&
            fm_layer_of(k) == layer)
            surf[ns++] = k;
    for (int i = 1; i < ns; i++) {
        fm_node *x = surf[i];
        int j = i - 1;
        while (j >= 0 && surf[j]->st.z > x->st.z) {
            surf[j + 1] = surf[j];
            j--;
        }
        surf[j + 1] = x;
    }
    for (int i = 0; i < ns; i++) {
        float e = arrival(d, i);
        if (e <= 0)
            continue;
        fmi_transform(surf[i], 1, 0, (1 - e) * 14);
        if (layer == 0 && d->nocc && e >= 1) { /* wholly under a window that hides it (with its shadow and glow) */
            const fm_style *s = &surf[i]->st;
            float sc = surf[i]->vscale > 0 ? surf[i]->vscale : 1;
            int m = (int)(fmaxf(s->shadow_blur + fmaxf(fabsf(s->shadow_x), fabsf(s->shadow_y)), s->glow_size) * sc) + 4;
            for (int k = 0; k < s->nshadow; k++)
                m = (int)fmaxf((float)m, (s->sh_blur[k] + s->sh_spread[k] + fmaxf(fabsf(s->sh_x[k]), fabsf(s->sh_y[k]))) * sc + 4);
            rect v = surf[i]->vis;
            v = (rect){v.x - m, v.y - m, v.w + 2 * m, v.h + 2 * m};
            bool hidden = false;
            for (int k = 0; k < d->nocc && !hidden; k++) {
                rect o = d->occ[k];
                hidden = v.x >= o.x && v.y >= o.y && v.x + v.w <= o.x + o.w && v.y + v.h <= o.y + o.h;
            }
            if (hidden)
                continue;
        }
        uint64_t t0 = prof_now_us();
        /* the lowest surface of the desktop, nothing but the wallpaper beneath: a canvas there may keep its picture */
        extern uint64_t fmi_canvas_backdrop;
        fmi_canvas_backdrop = layer == 0 && i == 0 && ds->bg.kind == FM_BG_NONE && e >= 1 && d == fm_host_desktop_doc()
                                  ? fm_host_backdrop_sig()
                                  : 0;
        if (d == fm_host_desktop_doc() && layer == 0)
            frame_timed = surf[i], timed_parts = surface_parts;
        draw_node(d, c, surf[i], e);
        frame_timed = NULL, timed_parts = frame_parts;
        fmi_canvas_backdrop = 0;
        if (d == fm_host_desktop_doc()) /* (the desktop's own surfaces, by id, for the log's "time:" line) */
            prof_add(surf[i]->id[0] ? surf[i]->id : surf[i]->tag, prof_now_us() - t0);
    }
}

void fm_set_occluders(fm_doc *d, const rect *r, int n) {
    if (!d)
        return;
    d->nocc = r && n > 0 ? (n < 24 ? n : 24) : 0;
    for (int i = 0; i < d->nocc; i++)
        d->occ[i] = r[i];
}

/* ---- window frames ---- */

bool fm_frame_opaque(fm_doc *d, float *radius) {
    fm_node *root = d && d->window_tpl ? fmi_tpl_root(d) : NULL;
    if (!root || !root->laid_out)
        return false;
    const fm_style *s = &root->st;
    float rmax = 0;
    for (int k = 0; k < 4; k++)
        rmax = fmaxf(rmax, s->radius[k]);
    *radius = rmax;
    /* (a blur under an opaque background shows nothing: glass or not, it hides what's behind) */
    return s->bg.kind == FM_BG_SOLID && C_A(s->bg.a) == 255 && s->opacity >= 0.999f && !s->bg_img[0];
}

static const char *const default_frame =
    "<frame>"
    "<titlebar drag=\"window\"><text bind=\"window.title\"/><spacer/>"
    "<button what=\"window:minimize\" icon=\"minimize\"/><button what=\"window:close\" icon=\"close\"/></titlebar>"
    "<slot/></frame>";

fm_node *fmi_tpl_root(fm_doc *d) {
    if (d->window_tpl) { /* still in the document? (an edit may have taken it out, and with it the memory) */
        bool there = false;
        for (fm_node *t = d->root ? d->root->first : NULL; t && !there; t = t->next)
            there = t == d->window_tpl;
        if (!there)
            d->window_tpl = NULL;
    }
    if (!d->window_tpl) {
        fm_node *t = fm_node_new(d, "template");
        t->generated = true;
        fm_attr_set(t, "id", "window");
        fm_append(d->root, t);
        fm_parse_fragment(d, t, default_frame);
        d->window_tpl = t;
    }
    for (fm_node *k = d->window_tpl->first; k; k = k->next)
        if (k->kind == FM_ELEM)
            return k;
    return NULL;
}




static fm_node *find_tag(fm_node *n, const char *tag) {
    for (; n; n = n->next) {
        if (tag_is(n, tag))
            return n;
        fm_node *r = find_tag(n->first, tag);
        if (r)
            return r;
    }
    return NULL;
}

static void clear_hover(fm_node *n) {
    for (; n; n = n->next) {
        n->hover = n->active = false;
        clear_hover(n->first);
    }
}

static void unanimate(fm_node *n) {
    for (; n; n = n->next) {
        n->av.init = false;
        unanimate(n->first);
    }
}

/* Lays the window template out at r for window `win`. */
/* A template of its own for this app's windows: <template id="window" for="terminal notes"> (or id="window-terminal") */
static fm_node *tpl_for_app(fm_doc *d, const char *app) {
    if (!app || !app[0] || !d->root)
        return NULL;
    for (fm_node *t = d->root->first; t; t = t->next) {
        if (t->kind != FM_ELEM || strcmp(t->tag, "template"))
            continue;
        const char *id = fm_attr_get(t, "id"), *fr = fm_attr_get(t, "for");
        bool mine = false;
        if (id && !strncmp(id, "window-", 7) && !strcmp(id + 7, app))
            mine = true;
        if (fr) {
            size_t l = strlen(app);
            for (const char *p = fr; (p = strstr(p, app)); p += l)
                if ((p == fr || p[-1] == ' ' || p[-1] == ',') && (!p[l] || p[l] == ' ' || p[l] == ','))
                    mine = true;
        }
        if (mine)
            for (fm_node *k = t->first; k; k = k->next)
                if (k->kind == FM_ELEM)
                    return k;
    }
    return NULL;
}

static fm_node *last_frame; /* the frame laid out last (an app's own template, or the usual one) */

fm_node *fmi_frame_layout(fm_doc *d, int win, rect r, bool focused, rect *content) {
    int ai = win >= 0 ? fm_host_window_app(win) : -1;
    const char *app = ai >= 0 ? fm_host_app_id(ai) : NULL;
    fm_node *root = tpl_for_app(d, app);
    if (!root)
        root = fmi_tpl_root(d);
    if (!root)
        return NULL;
    /* which app it frames, for frame[app="terminal"] and the like */
    fm_attr_set(root, "app", app ? app : "");
    last_frame = root;
    const char *title = win >= 0 ? fm_host_window_title(win) : NULL;
    fm_attr_set(root, "title", title ? title : "");
    d->cur_win = win;
    d->frame_focused = focused;
    if (d->hover_win != win)
        clear_hover(root);
    root->focus = focused;
    root->open = focused;
    fmi_sync_subtree(d, root);
    fmi_style_subtree(d, root);
    unanimate(root);
    fmi_arrange(d, root, r);
    fmi_transform(root, 1, 0, 0);
    fm_node *slot = find_tag(root->first, "slot");
    if (content)
        *content = slot && slot->laid_out
                       ? (rect){slot->box.x + (int)slot->st.pad[3], slot->box.y + (int)slot->st.pad[0],
                                slot->box.w - (int)(slot->st.pad[1] + slot->st.pad[3]),
                                slot->box.h - (int)(slot->st.pad[0] + slot->st.pad[2])}
                       : rect_inset(r, 8);
    return root;
}

/* Is n the frame of one of d's window templates now? (last_frame may be from a document since freed, a draft the
 * next one replaced, or a template an edit took out: read, it crashed the desktop.) Compared, never followed. */
static bool frame_of(fm_doc *d, const fm_node *n) {
    if (!n || !d || !d->root)
        return false;
    for (fm_node *t = d->root->first; t; t = t->next) {
        if (t->kind != FM_ELEM || strcmp(t->tag, "template"))
            continue;
        for (fm_node *k = t->first; k; k = k->next)
            if (k == n)
                return true;
    }
    return false;
}

bool fm_window_slot(fm_doc *d, rect *frame, float radius[4], float *border, rect *slot, rect *content) {
    fm_node *root = d && d->window_tpl ? (frame_of(d, last_frame) ? last_frame : fmi_tpl_root(d)) : NULL;
    fm_node *s = root ? find_tag(root->first, "slot") : NULL;
    if (!s || !s->laid_out || !root->laid_out)
        return false;
    *frame = root->box;
    memcpy(radius, root->st.radius, sizeof root->st.radius);
    *border = root->st.border_w;
    *slot = s->box;
    *content = (rect){s->box.x + (int)s->st.pad[3], s->box.y + (int)s->st.pad[0],
                      s->box.w - (int)(s->st.pad[1] + s->st.pad[3]), s->box.h - (int)(s->st.pad[0] + s->st.pad[2])};
    return true;
}

void fm_render_window_frame(fm_doc *d, canvas *c, int win, rect r, bool focused, rect *content_out, int opaque_w,
                            int opaque_h) {
    double l0 = pnow();
    rect content = {0, 0, 0, 0};
    fm_node *root = fmi_frame_layout(d, win, r, focused, &content);
    fm_pacc[5] += pnow() - l0;
    if (content_out)
        *content_out = content;
    if (opaque_w == 0) /* what the app fills next: its glass is blurred only around it (gfx.c) */
        blur_hole = content;
    else if (opaque_w > 0) /* as far as its opaque picture reaches, from the content's corner */
        blur_hole = (rect){content.x, content.y, opaque_w < content.w ? opaque_w : content.w,
                           opaque_h < content.h ? opaque_h : content.h};
    frame_timed = opaque_w >= 0 ? root : NULL;
    if (root)
        draw_node(d, c, root, 1);
    frame_timed = NULL;
    blur_hole = (rect){0, 0, 0, 0};
    d->cur_win = -1;
}
