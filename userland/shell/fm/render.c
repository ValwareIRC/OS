#define _GNU_SOURCE
#include "fmi.h"
#include "../iconart.h"

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

static void animate_node(fm_node *n, float dt, bool morphing) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        float k = n->st.transition <= 1 ? 1 : 1 - expf(-dt / (n->st.transition * 0.35f));
        if (morphing) /* one version of the desktop turning into another */
            k = 1 - expf(-dt / 150.0f);
        float kf = 1 - expf(-dt / 60.0f);
        n->a_hover = approach(n->a_hover, n->hover ? 1 : 0, kf);
        n->a_press = approach(n->a_press, n->active ? 1 : 0, kf);
        n->a_open = approach(n->a_open, n->open ? 1 : 0, 1 - expf(-dt / 70.0f));
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
    if (d->desktop) {
        animate_node(d->desktop, dt_ms, morphing);
        glide(d->desktop, 1 - expf(-dt_ms / 150.0f), morphing);
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

static color shade_grad(void *ctx, int x, int y) {
    struct grad *g = ctx;
    float t;
    if (g->bg.kind == FM_BG_RADIAL) {
        float cx = g->r.x + g->r.w / 2.0f, cy = g->r.y + g->r.h / 2.0f;
        float rx = (x + 0.5f - cx) / (g->r.w / 2.0f), ry = (y + 0.5f - cy) / (g->r.h / 2.0f);
        t = sqrtf(rx * rx + ry * ry);
    } else {
        float px = x + 0.5f - (g->r.x + g->r.w / 2.0f), py = y + 0.5f - (g->r.y + g->r.h / 2.0f);
        t = 0.5f + (px * g->dx + py * g->dy) / g->len;
    }
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return color_alpha(color_mix(g->bg.a, g->bg.b, t), g->op);
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
    if (bg.kind == FM_BG_LINEAR && fabsf(g.dx) < 1e-3f && same_radii(rad)) { /* straight down or up: one colour a row */
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

static void draw_text_node(canvas *c, fm_node *t, float op) {
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

static void draw_node(fm_doc *d, canvas *c, fm_node *n, float alpha);

static void draw_children(fm_doc *d, canvas *c, fm_node *n, float op) {
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
            draw_text_node(c, kids[i], op);
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
static void draw_node(fm_doc *d, canvas *c, fm_node *n, float alpha) {
    const fm_style *s = &n->st;
    double t0 = pnow();
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
    if (s->shadow_blur > 0 && C_A(sh))
        shadow_round(c, (rect){r.x + (int)(s->shadow_x * sc), r.y + (int)(s->shadow_y * sc), r.w, r.h}, r0,
                     s->shadow_blur * sc, color_alpha(sh, op));
    double t1 = pnow(); fm_pacc[0] += t1 - t0;
    float gs = anim ? n->av.glow_size : s->glow_size;
    if (gs > 0.5f)
        glow_round(c, r, r0, gs * sc, color_alpha(anim ? n->av.glow : s->glow, op));

    /* glass: blur and lift what's behind */
    double t2 = pnow(); fm_pacc[1] += t2 - t1;
    if (s->blur > 0.5f && op > 0.3f)
        blur_fast(c, r, r0, (int)fminf(s->blur, 40));
    double t3 = pnow(); fm_pacc[2] += t3 - t2;
    if (s->saturate > 1.02f && op > 0.3f)
        vibrancy(c, r, r0, s->saturate, 0.02f);
    double t4 = pnow(); fm_pacc[3] += t4 - t3;
    paint_bg(c, r, rad, bg, op);
    double t5 = pnow(); fm_pacc[4] += t5 - t4;
    /* glass catches light along its top edge */
    if (s->blur > 0.5f && op > 0.3f && r.h > 12)
        stroke_round_vgradient(c, rect_inset(r, 1), r0 > 1 ? r0 - 1 : 0, 1, RGBA(255, 255, 255, (int)(46 * op)),
                               RGBA(255, 255, 255, 0));

    if (s->border_w > 0) {
        float bw = s->border_w * sc;
        if (s->border_bg.kind == FM_BG_LINEAR)
            stroke_round_vgradient(c, r, r0, bw, color_alpha(s->border_bg.a, op), color_alpha(s->border_bg.b, op));
        else
            stroke_round(c, r, r0, bw, color_alpha(anim ? n->av.border : s->border, op));
    }

    /* content */
    if (tag_is(n, "icon")) {
        draw_icon_el(c, n, op);
    } else if (tag_is(n, "orb")) {
        fm_host_draw_orb(r, n->hover || n->open);
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
    } else if (tag_is(n, "divider")) {
        if (bg.kind == FM_BG_NONE)
            fill_rect(c, r, color_alpha(RGBA(255, 255, 255, 50), op));
    }

    bool clip = s->overflow != FM_OV_VISIBLE;
    clipbox cb = {0};
    if (clip)
        cb = clip_push(c, r);
    draw_children(d, c, n, op);
    if (clip)
        clip_pop(c, cb);

    /* the text caret of a focused field */
    if (d->focus == n && (tag_is(n, "search") || tag_is(n, "input"))) {
        int x = r.x + (int)(s->pad[3] * sc);
        for (fm_node *k = n->first; k; k = k->next)
            if (k->kind == FM_TEXT && k->generated && k->app != -2)
                x = k->vis.x + k->vis.w + 1;
        if ((time(NULL) & 1) == 0 || 1) {
            int lh = (int)(s->font_size * sc * 1.2f);
            fill_rect(c, (rect){x, r.y + (r.h - lh) / 2, (int)fmaxf(1.5f, sc * 1.5f), lh},
                      color_alpha(anim ? n->av.color : s->color, op));
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
        draw_node(d, c, surf[i], e);
    }
}

/* ---- window frames ---- */

static const char *const default_frame =
    "<frame>"
    "<titlebar drag=\"window\"><text bind=\"window.title\"/><spacer/>"
    "<button what=\"window:minimize\" icon=\"minimize\"/><button what=\"window:close\" icon=\"close\"/></titlebar>"
    "<slot/></frame>";

fm_node *fmi_tpl_root(fm_doc *d) {
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
fm_node *fmi_frame_layout(fm_doc *d, int win, rect r, bool focused, rect *content) {
    fm_node *root = fmi_tpl_root(d);
    if (!root)
        return NULL;
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

bool fm_window_slot(fm_doc *d, rect *frame, float radius[4], float *border, rect *slot, rect *content) {
    fm_node *root = d && d->window_tpl ? fmi_tpl_root(d) : NULL;
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

void fm_render_window_frame(fm_doc *d, canvas *c, int win, rect r, bool focused, rect *content_out) {
    double l0 = pnow();
    fm_node *root = fmi_frame_layout(d, win, r, focused, content_out);
    fm_pacc[5] += pnow() - l0;
    if (root)
        draw_node(d, c, root, 1);
    d->cur_win = -1;
}
