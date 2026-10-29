#include "iconart.h"
#include "design.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Colour icons for the built-in apps and the kinds of file. Each one is a
 * few shapes (rounded boxes, circles, strokes, polygons, arcs) measured as
 * signed distances, so edges are anti-aliased at any size, painted with
 * gradients and highlights over a soft shadow, in the spirit of macOS Big
 * Sur and Fluent: an object with a little depth, lit from above.
 *
 * Shapes are written in a unit square (0..1, y down). Everything is
 * rendered once per subject, look and size into a small premultiplied
 * picture, kept in a cache and blended onto the frame from there. */

#define PI 3.14159265f
#define HEX(x) RGBA(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255, 255)
#define HEXA(x, a) RGBA(((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255, a)
#define WHITE HEX(0xffffff)

/* ---- shapes ---- */

enum { P_RBOX, P_CIRC, P_SEG, P_POLY, P_ARC, P_ELL };

typedef struct prim {
    int kind, cut, n;
    float a[16];
} prim;

typedef struct shape {
    int n;
    prim v[14];
} shape;

static prim rbox(float x0, float y0, float x1, float y1, float r) { return (prim){P_RBOX, 0, 5, {x0, y0, x1, y1, r}}; }
static prim circ(float cx, float cy, float r) { return (prim){P_CIRC, 0, 3, {cx, cy, r}}; }
static prim seg(float x0, float y0, float x1, float y1, float w) { return (prim){P_SEG, 0, 5, {x0, y0, x1, y1, w}}; }
static prim ell(float cx, float cy, float rx, float ry) { return (prim){P_ELL, 0, 4, {cx, cy, rx, ry}}; }
/* A stroke of width w along a circle, from angle a0 to a1 (radians, a1 > a0, y down) */
static prim arc(float cx, float cy, float rad, float w, float a0, float a1) {
    return (prim){P_ARC, 0, 6, {cx, cy, rad, w, a0, a1}};
}
static prim poly(int np, const float *pts) {
    prim p = {P_POLY, 0, np > 8 ? 8 : np, {0}};
    memcpy(p.a, pts, sizeof(float) * 2 * (size_t)p.n);
    return p;
}
static prim cut(prim p) {
    p.cut = 1;
    return p;
}

/* A shape from prims, in order: each one is added, or cut out if cut(). */
static shape shp(int n, ...) {
    shape s;
    s.n = 0;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && s.n < 14; i++)
        s.v[s.n++] = va_arg(ap, prim);
    va_end(ap);
    return s;
}
static shape one(prim p) { return shp(1, p); }
static void add(shape *s, prim p) {
    if (s->n < 14)
        s->v[s->n++] = p;
}

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static float prim_sd(const prim *p, float x, float y) {
    const float *a = p->a;
    switch (p->kind) {
    case P_RBOX: {
        float cx = (a[0] + a[2]) / 2, cy = (a[1] + a[3]) / 2, hx = (a[2] - a[0]) / 2, hy = (a[3] - a[1]) / 2;
        float r = fminf(a[4], fminf(hx, hy));
        float qx = fabsf(x - cx) - hx + r, qy = fabsf(y - cy) - hy + r;
        return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
    }
    case P_CIRC:
        return hypotf(x - a[0], y - a[1]) - a[2];
    case P_SEG: {
        float px = x - a[0], py = y - a[1], bx = a[2] - a[0], by = a[3] - a[1];
        float bb = bx * bx + by * by, h = bb > 0 ? clampf((px * bx + py * by) / bb) : 0;
        return hypotf(px - bx * h, py - by * h) - a[4] / 2;
    }
    case P_ELL: {
        float px = x - a[0], py = y - a[1];
        float k0 = hypotf(px / a[2], py / a[3]), k1 = hypotf(px / (a[2] * a[2]), py / (a[3] * a[3]));
        return k1 > 1e-6f ? k0 * (k0 - 1) / k1 : -fminf(a[2], a[3]);
    }
    case P_ARC: {
        float dx = x - a[0], dy = y - a[1];
        float t = atan2f(dy, dx) - a[4];
        t -= floorf(t / (2 * PI)) * 2 * PI;
        if (t <= a[5] - a[4])
            return fabsf(hypotf(dx, dy) - a[2]) - a[3] / 2;
        float e0 = hypotf(x - (a[0] + a[2] * cosf(a[4])), y - (a[1] + a[2] * sinf(a[4])));
        float e1 = hypotf(x - (a[0] + a[2] * cosf(a[5])), y - (a[1] + a[2] * sinf(a[5])));
        return fminf(e0, e1) - a[3] / 2;
    }
    case P_POLY: { /* distance to the nearest edge, signed by crossings */
        int n = p->n;
        float d = (x - a[0]) * (x - a[0]) + (y - a[1]) * (y - a[1]), s = 1;
        for (int i = 0, j = n - 1; i < n; j = i, i++) {
            float vix = a[2 * i], viy = a[2 * i + 1], vjx = a[2 * j], vjy = a[2 * j + 1];
            float ex = vjx - vix, ey = vjy - viy, wx = x - vix, wy = y - viy;
            float ee = ex * ex + ey * ey, h = ee > 0 ? clampf((wx * ex + wy * ey) / ee) : 0;
            float bx = wx - ex * h, by = wy - ey * h;
            d = fminf(d, bx * bx + by * by);
            bool c1 = y >= viy, c2 = y < vjy, c3 = ex * wy > ey * wx;
            if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
                s = -s;
        }
        return s * sqrtf(d);
    }
    }
    return 1e9f;
}

static float shape_sd(const shape *s, float x, float y) {
    float d = 1e9f;
    for (int i = 0; i < s->n; i++) {
        float pd = prim_sd(&s->v[i], x, y);
        d = s->v[i].cut ? fmaxf(d, -pd) : fminf(d, pd);
    }
    return d;
}

static void shape_bbox(const shape *s, float *x0, float *y0, float *x1, float *y1) {
    *x0 = *y0 = 1e9f;
    *x1 = *y1 = -1e9f;
    for (int i = 0; i < s->n; i++) {
        const prim *p = &s->v[i];
        const float *a = p->a;
        float l, t, r, b;
        if (p->cut)
            continue;
        switch (p->kind) {
        case P_RBOX: l = a[0], t = a[1], r = a[2], b = a[3]; break;
        case P_CIRC: l = a[0] - a[2], t = a[1] - a[2], r = a[0] + a[2], b = a[1] + a[2]; break;
        case P_SEG:
            l = fminf(a[0], a[2]) - a[4], r = fmaxf(a[0], a[2]) + a[4];
            t = fminf(a[1], a[3]) - a[4], b = fmaxf(a[1], a[3]) + a[4];
            break;
        case P_ELL: l = a[0] - a[2], t = a[1] - a[3], r = a[0] + a[2], b = a[1] + a[3]; break;
        case P_ARC: l = a[0] - a[2] - a[3], t = a[1] - a[2] - a[3], r = a[0] + a[2] + a[3], b = a[1] + a[2] + a[3]; break;
        default:
            l = t = 1e9f, r = b = -1e9f;
            for (int k = 0; k < p->n; k++) {
                l = fminf(l, a[2 * k]), r = fmaxf(r, a[2 * k]);
                t = fminf(t, a[2 * k + 1]), b = fmaxf(b, a[2 * k + 1]);
            }
        }
        *x0 = fminf(*x0, l), *y0 = fminf(*y0, t), *x1 = fmaxf(*x1, r), *y1 = fmaxf(*y1, b);
    }
}

/* ---- paint ---- */

enum { PAINT_SOLID, PAINT_LINEAR, PAINT_RADIAL };
#define NORIM 1 /* classic: no dark outline (details, highlights) */
#define RAW 2   /* used as is in every look (tiles that already are in the tint) */

typedef struct paint {
    int kind, flags;
    color c0, c1;
    float x0, y0, x1, y1;
} paint;

static paint solid(color c) { return (paint){PAINT_SOLID, 0, c, c, 0, 0, 0, 0}; }
static paint vgrad(color top, color bottom, float y0, float y1) { return (paint){PAINT_LINEAR, 0, top, bottom, 0, y0, 0, y1}; }
static paint lgrad(color c0, color c1, float x0, float y0, float x1, float y1) {
    return (paint){PAINT_LINEAR, 0, c0, c1, x0, y0, x1, y1};
}
static paint rgrad(color inner, color outer, float cx, float cy, float r) {
    return (paint){PAINT_RADIAL, 0, inner, outer, cx, cy, r, 0};
}
static paint flag(paint p, int f) {
    p.flags |= f;
    return p;
}

static color paint_at(const paint *p, float x, float y) {
    float t = 0;
    if (p->kind == PAINT_LINEAR) {
        float dx = p->x1 - p->x0, dy = p->y1 - p->y0, dd = dx * dx + dy * dy;
        t = dd > 0 ? ((x - p->x0) * dx + (y - p->y0) * dy) / dd : 0;
    } else if (p->kind == PAINT_RADIAL) {
        t = p->x1 > 0 ? hypotf(x - p->x0, y - p->y0) / p->x1 : 0;
    } else {
        return p->c0;
    }
    return color_mix(p->c0, p->c1, clampf(t));
}

/* ---- the picture being drawn ---- */

typedef struct ib {
    int n;
    float *p; /* n*n premultiplied r, g, b, a (0..1) */
    enum icon_look look;
    color tint;
    float ox, oy, k; /* unit square -> pixels: x = ox + u * k */
} ib;

/* The look's version of a colour: mono keeps only how light it is and
 * says it in the tint; classic is a little more saturated. */
static color look_color(const ib *b, color c) {
    if (b->look == LOOK_MONO) {
        float l = (0.299f * C_R(c) + 0.587f * C_G(c) + 0.114f * C_B(c)) / 255.0f;
        color away = color_luma(b->tint) < 0.4f ? WHITE : HEX(0x000000);
        color m = color_mix(b->tint, away, l * 0.62f);
        return (m & 0x00ffffffu) | (c & 0xff000000u);
    }
    if (b->look == LOOK_CLASSIC) {
        float g = (0.299f * C_R(c) + 0.587f * C_G(c) + 0.114f * C_B(c));
        float k = 1.3f;
        int r = (int)(g + (C_R(c) - g) * k), gg = (int)(g + (C_G(c) - g) * k), bb = (int)(g + (C_B(c) - g) * k);
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
        bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
        return RGBA(r, gg, bb, C_A(c));
    }
    return c;
}

enum { M_FILL, M_STROKE, M_SHADOW };

static void layer(ib *b, const shape *s, const shape *clip, paint pt, int mode, float w, float dy, float blur) {
    float x0, y0, x1, y1;
    shape_bbox(s, &x0, &y0, &x1, &y1);
    float grow = mode == M_SHADOW ? blur + fabsf(dy) : w;
    int i0 = (int)floorf(b->ox + (x0 - grow) * b->k) - 2, i1 = (int)ceilf(b->ox + (x1 + grow) * b->k) + 2;
    int j0 = (int)floorf(b->oy + (y0 - grow) * b->k) - 2, j1 = (int)ceilf(b->oy + (y1 + grow) * b->k) + 2;
    i0 = i0 < 0 ? 0 : i0, j0 = j0 < 0 ? 0 : j0;
    i1 = i1 > b->n ? b->n : i1, j1 = j1 > b->n ? b->n : j1;
    bool rim = mode == M_FILL && b->look == LOOK_CLASSIC && !(pt.flags & NORIM);
    for (int j = j0; j < j1; j++)
        for (int i = i0; i < i1; i++) {
            float u = (i + 0.5f - b->ox) / b->k, v = (j + 0.5f - b->oy) / b->k, cov, d = 0;
            if (mode == M_SHADOW) {
                float t = clampf(0.5f - shape_sd(s, u, v - dy) / blur);
                cov = t * t * (3 - 2 * t);
            } else if (mode == M_STROKE) {
                cov = clampf(0.5f - (fabsf(shape_sd(s, u, v)) - w / 2) * b->k);
            } else {
                d = shape_sd(s, u, v) * b->k;
                cov = clampf(0.5f - d);
            }
            if (cov <= 0)
                continue;
            if (clip)
                cov *= clampf(0.5f - shape_sd(clip, u, v) * b->k);
            if (cov <= 0)
                continue;
            color col = paint_at(&pt, u, v);
            if (!(pt.flags & RAW))
                col = look_color(b, col);
            float a = C_A(col) / 255.0f * cov, r = C_R(col) / 255.0f, g = C_G(col) / 255.0f, bl = C_B(col) / 255.0f;
            if (rim) { /* a dark line just inside the edge, like hand-drawn pixel icons */
                float e = (d >= 0 ? 1 : clampf(1 + d / 1.4f)) * 0.7f;
                r *= 1 - e * 0.6f, g *= 1 - e * 0.6f, bl *= 1 - e * 0.6f;
            }
            float *q = &b->p[((size_t)j * (size_t)b->n + (size_t)i) * 4];
            q[0] = r * a + q[0] * (1 - a);
            q[1] = g * a + q[1] * (1 - a);
            q[2] = bl * a + q[2] * (1 - a);
            q[3] = a + q[3] * (1 - a);
        }
}

static void fill(ib *b, shape s, paint p) { layer(b, &s, NULL, p, M_FILL, 0, 0, 0); }
static void fill_in(ib *b, shape s, shape clip, paint p) { layer(b, &s, &clip, p, M_FILL, 0, 0, 0); }
static void stroke(ib *b, shape s, float w, paint p) { layer(b, &s, NULL, p, M_STROKE, w, 0, 0); }
/* A soft shadow of s, dropped by dy. Mono icons have none; classic ones
 * have a short hard one, as icons did then. */
static void shadow(ib *b, shape s, float dy, float blur, int alpha) {
    if (b->look == LOOK_MONO)
        return;
    paint p = flag(solid(HEXA(0x000000, alpha)), RAW);
    if (b->look == LOOK_CLASSIC) {
        blur = fmaxf(blur * 0.35f, 1.5f / b->k);
        p.c0 = p.c1 = HEXA(0x000000, alpha + 30 > 255 ? 255 : alpha + 30);
        for (int i = 0; i < s.n; i++) { /* down and to the right */
            prim *v = &s.v[i];
            if (v->kind == P_POLY) {
                for (int k = 0; k < v->n; k++)
                    v->a[2 * k] += 0.02f;
            } else {
                v->a[0] += 0.02f;
                if (v->kind == P_RBOX || v->kind == P_SEG)
                    v->a[2] += 0.02f;
            }
        }
    }
    layer(b, &s, NULL, p, M_SHADOW, 0, dy, blur);
}

/* A soft white sheen across the top of a shape. */
static void sheen(ib *b, shape s, float y0, float y1, int alpha) {
    fill_in(b, one(rbox(-1, y0, 2, y1, 0)), s, flag(vgrad(HEXA(0xffffff, alpha), HEXA(0xffffff, 0), y0, y1), NORIM));
}

/* ---- the subjects ---- */

static void art_folder(ib *b) {
    shape whole = one(rbox(0.08f, 0.18f, 0.92f, 0.84f, 0.07f));
    shadow(b, whole, 0.035f, 0.07f, 70);
    shape back = shp(2, rbox(0.08f, 0.18f, 0.46f, 0.36f, 0.05f), rbox(0.08f, 0.26f, 0.92f, 0.84f, 0.07f));
    fill(b, back, vgrad(HEX(0x4b97f7), HEX(0x1f5fd0), 0.18f, 0.84f));
    /* a sheet of paper peeking out */
    fill(b, one(rbox(0.15f, 0.30f, 0.85f, 0.62f, 0.03f)), flag(vgrad(WHITE, HEX(0xdfe7f2), 0.3f, 0.6f), NORIM));
    shape front = one(rbox(0.08f, 0.38f, 0.92f, 0.84f, 0.07f));
    fill(b, front, vgrad(HEX(0x93cdff), HEX(0x3a8cf0), 0.38f, 0.84f));
    fill_in(b, one(rbox(0, 0.38f, 1, 0.405f, 0)), front, flag(solid(HEXA(0xffffff, 170)), NORIM));
    fill_in(b, one(rbox(0, 0.80f, 1, 0.84f, 0)), front, flag(solid(HEXA(0x0b3d91, 40)), NORIM));
}

static void sparkle(ib *b, float cx, float cy, float r, paint p) {
    float k = r * 0.24f;
    float pts[16] = {cx, cy - r, cx + k, cy - k, cx + r, cy, cx + k, cy + k,
                     cx, cy + r, cx - k, cy + k, cx - r, cy, cx - k, cy - k};
    fill(b, one(poly(8, pts)), p);
}

static void art_companion(ib *b) {
    shape orb = one(circ(0.5f, 0.5f, 0.39f));
    shadow(b, orb, 0.04f, 0.08f, 80);
    fill(b, orb, lgrad(HEX(0xff8fb1), HEX(0x6d4aff), 0.25f, 0.12f, 0.75f, 0.9f));
    fill(b, orb, flag(rgrad(HEXA(0xffc46b, 235), HEXA(0xffc46b, 0), 0.34f, 0.30f, 0.46f), NORIM));
    fill(b, orb, flag(rgrad(HEXA(0x3b1d8f, 0), HEXA(0x3b1d8f, 90), 0.5f, 0.42f, 0.5f), NORIM));
    fill_in(b, one(ell(0.42f, 0.27f, 0.22f, 0.12f)), orb, flag(vgrad(HEXA(0xffffff, 120), HEXA(0xffffff, 0), 0.15f, 0.39f), NORIM));
    sparkle(b, 0.5f, 0.53f, 0.21f, flag(solid(WHITE), NORIM));
    sparkle(b, 0.69f, 0.33f, 0.075f, flag(solid(HEXA(0xffffff, 230)), NORIM));
}

static void art_notes(ib *b) {
    shape page = one(rbox(0.16f, 0.09f, 0.84f, 0.91f, 0.08f));
    shadow(b, page, 0.03f, 0.06f, 70);
    fill(b, page, vgrad(WHITE, HEX(0xefeee9), 0.1f, 0.9f));
    fill_in(b, one(rbox(0, 0, 1, 0.29f, 0)), page, flag(vgrad(HEX(0xffd862), HEX(0xf6b400), 0.09f, 0.29f), NORIM));
    fill_in(b, one(rbox(0, 0.29f, 1, 0.305f, 0)), page, flag(solid(HEXA(0x8a5a00, 60)), NORIM));
    float ys[4] = {0.43f, 0.55f, 0.67f, 0.79f};
    for (int i = 0; i < 4; i++)
        fill(b, one(rbox(0.27f, ys[i], i == 3 ? 0.56f : 0.73f, ys[i] + 0.026f, 0.013f)), flag(solid(HEX(0xcfd3da)), NORIM));
    fill_in(b, one(rbox(0, 0.09f, 1, 0.2f, 0)), page, flag(vgrad(HEXA(0xffffff, 110), HEXA(0xffffff, 0), 0.09f, 0.2f), NORIM));
}

static shape gear(float cx, float cy, float r, float hole) {
    shape s = one(circ(cx, cy, r * 0.74f));
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4 + PI / 8, ca = cosf(a), sa = sinf(a), tx = -sa, ty = ca;
        float rb = r * 0.6f, rt = r, wb = r * 0.2f, wt = r * 0.14f;
        float pts[8] = {cx + ca * rb - tx * wb, cy + sa * rb - ty * wb, cx + ca * rt - tx * wt, cy + sa * rt - ty * wt,
                        cx + ca * rt + tx * wt, cy + sa * rt + ty * wt, cx + ca * rb + tx * wb, cy + sa * rb + ty * wb};
        add(&s, poly(4, pts));
    }
    if (hole > 0)
        add(&s, cut(circ(cx, cy, hole)));
    return s;
}

static void art_settings(ib *b) {
    shape g = gear(0.5f, 0.5f, 0.42f, 0.115f);
    shadow(b, g, 0.035f, 0.06f, 80);
    fill(b, g, lgrad(HEX(0xc9d0da), HEX(0x6a7586), 0.3f, 0.1f, 0.7f, 0.92f));
    sheen(b, g, 0.08f, 0.45f, 90);
    shape ringd = shp(2, circ(0.5f, 0.5f, 0.215f), cut(circ(0.5f, 0.5f, 0.115f)));
    fill(b, ringd, flag(lgrad(HEX(0x5d6879), HEX(0xaab3c0), 0.35f, 0.3f, 0.65f, 0.72f), NORIM));
    stroke(b, one(circ(0.5f, 0.5f, 0.215f)), 0.012f, flag(solid(HEXA(0xffffff, 110)), NORIM));
}

static void art_computer(ib *b) {
    float neck[8] = {0.43f, 0.68f, 0.57f, 0.68f, 0.6f, 0.83f, 0.4f, 0.83f};
    shadow(b, shp(3, rbox(0.07f, 0.13f, 0.93f, 0.71f, 0.06f), poly(4, neck), rbox(0.28f, 0.81f, 0.72f, 0.87f, 0.03f)), 0.03f,
           0.06f, 70);
    fill(b, one(poly(4, neck)), vgrad(HEX(0xb8bec8), HEX(0x8c94a1), 0.68f, 0.83f));
    fill(b, one(rbox(0.28f, 0.81f, 0.72f, 0.87f, 0.03f)), vgrad(HEX(0xe9ecf0), HEX(0xa3aab5), 0.81f, 0.87f));
    shape bezel = one(rbox(0.07f, 0.13f, 0.93f, 0.71f, 0.06f));
    fill(b, bezel, vgrad(HEX(0x454b57), HEX(0x1c1f26), 0.13f, 0.71f));
    shape screen = one(rbox(0.11f, 0.17f, 0.89f, 0.64f, 0.03f));
    fill(b, screen, flag(lgrad(HEX(0x4fd1f5), HEX(0x7a5cff), 0.1f, 0.15f, 0.9f, 0.66f), NORIM));
    /* a hill on the wallpaper, and the glass catching the light */
    fill_in(b, one(ell(0.36f, 0.78f, 0.42f, 0.24f)), screen, flag(solid(HEXA(0x2a1d8f, 70)), NORIM));
    fill_in(b, one(ell(0.76f, 0.74f, 0.36f, 0.2f)), screen, flag(solid(HEXA(0xffffff, 40)), NORIM));
    float glare[8] = {0.11f, 0.17f, 0.52f, 0.17f, 0.3f, 0.64f, 0.11f, 0.64f};
    fill_in(b, one(poly(4, glare)), screen, flag(solid(HEXA(0xffffff, 34)), NORIM));
    fill(b, one(circ(0.5f, 0.675f, 0.011f)), flag(solid(HEXA(0xffffff, 120)), NORIM));
}

static void art_terminal(ib *b) {
    shape win = one(rbox(0.07f, 0.13f, 0.93f, 0.87f, 0.1f));
    shadow(b, win, 0.035f, 0.07f, 80);
    fill(b, win, vgrad(HEX(0x3d4454), HEX(0x161a21), 0.13f, 0.87f));
    fill_in(b, one(rbox(0, 0, 1, 0.27f, 0)), win, flag(solid(HEXA(0xffffff, 26)), NORIM));
    stroke(b, win, 0.012f, flag(vgrad(HEXA(0xffffff, 70), HEXA(0xffffff, 8), 0.13f, 0.5f), NORIM | RAW));
    const unsigned dots[3] = {0xff5f57, 0xfebc2e, 0x28c840};
    for (int i = 0; i < 3; i++)
        fill(b, one(circ(0.175f + i * 0.075f, 0.2f, 0.024f)), flag(solid(HEX(dots[i])), NORIM));
    fill(b, shp(2, seg(0.22f, 0.43f, 0.35f, 0.535f, 0.06f), seg(0.35f, 0.535f, 0.22f, 0.64f, 0.06f)),
         flag(solid(HEX(0x5cf28f)), NORIM));
    fill(b, one(rbox(0.41f, 0.615f, 0.62f, 0.665f, 0.02f)), flag(solid(HEXA(0xe8edf3, 235)), NORIM));
}

static void art_appearance(ib *b) {
    shape pal = shp(2, ell(0.5f, 0.52f, 0.41f, 0.35f), cut(circ(0.63f, 0.71f, 0.075f)));
    shadow(b, pal, 0.035f, 0.07f, 70);
    fill(b, pal, lgrad(HEX(0xfff8ea), HEX(0xe2c99b), 0.25f, 0.2f, 0.75f, 0.9f));
    sheen(b, pal, 0.17f, 0.45f, 120);
    const struct {
        float x, y;
        unsigned c;
    } dots[5] = {{0.29f, 0.47f, 0xff4d4f}, {0.42f, 0.33f, 0xffb800}, {0.59f, 0.32f, 0x2fc35a},
                 {0.73f, 0.45f, 0x2f7bff}, {0.33f, 0.66f, 0xa04dff}};
    for (int i = 0; i < 5; i++) {
        shape d = one(circ(dots[i].x, dots[i].y, 0.068f));
        fill(b, d, vgrad(color_mix(HEX(dots[i].c), WHITE, 0.25f), color_mix(HEX(dots[i].c), HEX(0), 0.12f),
                         dots[i].y - 0.07f, dots[i].y + 0.07f));
        fill(b, one(circ(dots[i].x - 0.02f, dots[i].y - 0.022f, 0.02f)), flag(solid(HEXA(0xffffff, 150)), NORIM));
    }
}

static void art_power(ib *b) {
    shape btn = one(circ(0.5f, 0.5f, 0.39f));
    shadow(b, btn, 0.04f, 0.07f, 85);
    fill(b, btn, lgrad(HEX(0xff8070), HEX(0xcf2a1d), 0.3f, 0.1f, 0.7f, 0.92f));
    fill_in(b, one(ell(0.5f, 0.29f, 0.29f, 0.16f)), btn, flag(vgrad(HEXA(0xffffff, 120), HEXA(0xffffff, 0), 0.13f, 0.45f), NORIM));
    fill(b, shp(2, arc(0.5f, 0.53f, 0.175f, 0.065f, -PI / 2 + 0.7f, 1.5f * PI - 0.7f), seg(0.5f, 0.27f, 0.5f, 0.5f, 0.065f)),
         flag(solid(WHITE), NORIM));
}

/* A page with its top-right corner folded over. */
static shape page_shape(void) {
    float corner[6] = {0.6f, 0.07f, 0.83f, 0.07f, 0.83f, 0.29f};
    return shp(2, rbox(0.17f, 0.07f, 0.83f, 0.93f, 0.06f), cut(poly(3, corner)));
}

static void art_page(ib *b, color paper) {
    shape page = page_shape();
    shadow(b, page, 0.03f, 0.06f, 70);
    fill(b, page, vgrad(color_mix(paper, WHITE, 0.6f), paper, 0.07f, 0.93f));
    float flap[6] = {0.6f, 0.07f, 0.6f, 0.26f, 0.83f, 0.29f};
    float under[6] = {0.6f, 0.07f, 0.83f, 0.29f, 0.6f, 0.29f};
    fill(b, one(poly(3, under)), flag(solid(HEXA(0x000000, 18)), NORIM));
    fill(b, one(poly(3, flap)), lgrad(HEX(0xf4f6f9), HEX(0xc3cad5), 0.6f, 0.1f, 0.75f, 0.3f));
}

static void art_document(ib *b) {
    art_page(b, HEX(0xeef1f6));
    fill(b, one(rbox(0.27f, 0.38f, 0.58f, 0.42f, 0.02f)), flag(solid(HEX(0x3b7bf0)), NORIM));
    for (int i = 0; i < 4; i++) {
        float y = 0.5f + i * 0.085f;
        fill(b, one(rbox(0.27f, y, i == 3 ? 0.55f : 0.73f, y + 0.026f, 0.013f)), flag(solid(HEX(0xbfc6d1)), NORIM));
    }
}

static void art_image(ib *b) {
    art_page(b, HEX(0xeef1f6));
    shape photo = one(rbox(0.26f, 0.36f, 0.74f, 0.8f, 0.04f));
    fill(b, photo, flag(vgrad(HEX(0x5fb6ff), HEX(0xcbeaff), 0.36f, 0.72f), NORIM));
    fill(b, one(circ(0.62f, 0.48f, 0.055f)), flag(solid(HEX(0xffc93c)), NORIM));
    float hills[10] = {0.2f, 0.84f, 0.2f, 0.7f, 0.4f, 0.55f, 0.55f, 0.68f, 0.8f, 0.84f};
    fill_in(b, one(poly(5, hills)), photo, flag(vgrad(HEX(0x4cc276), HEX(0x22864a), 0.55f, 0.8f), NORIM));
    float hill2[8] = {0.48f, 0.84f, 0.62f, 0.62f, 0.8f, 0.76f, 0.8f, 0.84f};
    fill_in(b, one(poly(4, hill2)), photo, flag(vgrad(HEX(0x2f9d5c), HEX(0x1b6b3c), 0.62f, 0.8f), NORIM));
}

static void art_audio(ib *b) {
    art_page(b, HEX(0xf6eef2));
    float beam[8] = {0.42f, 0.4f, 0.69f, 0.34f, 0.69f, 0.42f, 0.42f, 0.48f};
    shape note = shp(5, circ(0.38f, 0.72f, 0.07f), circ(0.65f, 0.66f, 0.07f), rbox(0.42f, 0.4f, 0.46f, 0.72f, 0),
                     rbox(0.69f, 0.34f, 0.73f, 0.66f, 0), poly(4, beam));
    fill(b, note, lgrad(HEX(0xff6b9a), HEX(0xc2255c), 0.3f, 0.35f, 0.7f, 0.8f));
}

static void art_video(ib *b) {
    art_page(b, HEX(0xf0eef8));
    shape screen = one(rbox(0.25f, 0.4f, 0.75f, 0.78f, 0.06f));
    fill(b, screen, lgrad(HEX(0xa77bff), HEX(0x5a37d6), 0.25f, 0.4f, 0.75f, 0.8f));
    float play[6] = {0.45f, 0.5f, 0.45f, 0.68f, 0.6f, 0.59f};
    fill(b, one(poly(3, play)), flag(solid(WHITE), NORIM));
}

static void art_archive(ib *b) {
    art_page(b, HEX(0xf5eddc));
    for (int i = 0; i < 7; i++) {
        float y = 0.1f + i * 0.06f, x = i & 1 ? 0.5f : 0.44f;
        fill(b, one(rbox(x, y, x + 0.06f, y + 0.035f, 0.01f)), flag(solid(HEX(0x8d7a5c)), NORIM));
    }
    shape tab = one(rbox(0.4f, 0.54f, 0.6f, 0.74f, 0.04f));
    fill(b, tab, vgrad(HEX(0xffc65a), HEX(0xd9881a), 0.54f, 0.74f));
    fill(b, one(rbox(0.46f, 0.6f, 0.54f, 0.69f, 0.02f)), flag(solid(HEXA(0x6b3d00, 120)), NORIM));
}

static void art_code(ib *b) {
    art_page(b, HEX(0xeaf2f7));
    paint ink = flag(lgrad(HEX(0x2bb3e8), HEX(0x2563eb), 0.3f, 0.4f, 0.7f, 0.8f), NORIM);
    fill(b, shp(2, seg(0.38f, 0.48f, 0.27f, 0.59f, 0.055f), seg(0.27f, 0.59f, 0.38f, 0.7f, 0.055f)), ink);
    fill(b, shp(2, seg(0.62f, 0.48f, 0.73f, 0.59f, 0.055f), seg(0.73f, 0.59f, 0.62f, 0.7f, 0.055f)), ink);
    fill(b, one(seg(0.55f, 0.43f, 0.45f, 0.75f, 0.05f)), flag(solid(HEX(0x8a96a8)), NORIM));
}

static void art_executable(ib *b) {
    shape win = one(rbox(0.1f, 0.16f, 0.9f, 0.84f, 0.08f));
    shadow(b, win, 0.03f, 0.06f, 70);
    fill(b, win, vgrad(HEX(0xf7f9fc), HEX(0xd5dbe4), 0.16f, 0.84f));
    fill_in(b, one(rbox(0, 0, 1, 0.31f, 0)), win, flag(vgrad(HEX(0x5a95fa), HEX(0x2b62e3), 0.16f, 0.31f), NORIM));
    for (int i = 0; i < 3; i++)
        fill(b, one(circ(0.19f + i * 0.06f, 0.235f, 0.018f)), flag(solid(HEXA(0xffffff, 200)), NORIM));
    shape g = gear(0.5f, 0.58f, 0.17f, 0.05f);
    fill(b, g, lgrad(HEX(0x7f8a9c), HEX(0x4a5466), 0.4f, 0.42f, 0.6f, 0.76f));
}

static void draw_subject(ib *b, int subject) {
    switch (subject) {
    case APP_FILES:
    case ICON_FOLDER: art_folder(b); break;
    case APP_COMPANION: art_companion(b); break;
    case APP_NOTES: art_notes(b); break;
    case APP_SETTINGS: art_settings(b); break;
    case APP_ABOUT: art_computer(b); break;
    case APP_TERMINAL: art_terminal(b); break;
    case APP_REDESIGN: art_appearance(b); break;
    case APP_POWER: art_power(b); break;
    case ICON_DOCUMENT: art_document(b); break;
    case ICON_IMAGE: art_image(b); break;
    case ICON_AUDIO: art_audio(b); break;
    case ICON_VIDEO: art_video(b); break;
    case ICON_ARCHIVE: art_archive(b); break;
    case ICON_CODE: art_code(b); break;
    default: art_executable(b); break;
    }
}

/* The colour each subject is known by: its tile is tinted with it. */
static color hue_of(int subject) {
    switch (subject) {
    case APP_FILES:
    case ICON_FOLDER: return HEX(0x3b8ef0);
    case APP_COMPANION: return HEX(0x9b5cf6);
    case APP_NOTES: return HEX(0xf5b800);
    case APP_SETTINGS: return HEX(0x7b8696);
    case APP_ABOUT: return HEX(0x3fb5e8);
    case APP_TERMINAL: return HEX(0x3a4252);
    case APP_REDESIGN: return HEX(0xff8a3d);
    case APP_POWER: return HEX(0xe5483a);
    case ICON_IMAGE: return HEX(0x34a86b);
    case ICON_AUDIO: return HEX(0xe8457a);
    case ICON_VIDEO: return HEX(0x7b4dff);
    case ICON_ARCHIVE: return HEX(0xc98a2b);
    case ICON_CODE: return HEX(0x1f9bd1);
    default: return HEX(0x4a7bd6);
    }
}

/* The tile behind the object when the design asks for icon-shape. */
static void draw_tile(ib *b, int subject, int tile) {
    float x0 = 0.03f, y0 = 0.02f, x1 = 0.97f, y1 = 0.95f;
    float rad = tile == 1 ? 0.5f : tile == 2 ? 0.235f : 0.1f;
    shape t = tile == 1 ? one(circ(0.5f, 0.485f, 0.465f)) : one(rbox(x0, y0, x1, y1, rad));
    color hue = hue_of(subject);
    if (b->look == LOOK_MONO) {
        fill(b, t, flag(solid(color_alpha(b->tint, 0.14f)), RAW | NORIM));
        stroke(b, t, 1.2f / b->k, flag(solid(color_alpha(b->tint, 0.4f)), RAW | NORIM));
        return;
    }
    shadow(b, t, 0.025f, 0.05f, 70);
    if (b->look == LOOK_CLASSIC) { /* a glossy XP button */
        fill(b, t, vgrad(color_mix(hue, WHITE, 0.45f), color_mix(hue, HEX(0), 0.1f), y0, y1));
        fill_in(b, one(ell(0.5f, 0.16f, 0.62f, 0.3f)), t, flag(vgrad(HEXA(0xffffff, 170), HEXA(0xffffff, 40), 0, 0.46f), NORIM));
        return;
    }
    fill(b, t, flag(vgrad(color_mix(WHITE, hue, 0.04f), color_mix(WHITE, hue, 0.2f), y0, y1), RAW));
    fill_in(b, one(rbox(-1, y0, 2, 0.5f, 0)), t, flag(vgrad(HEXA(0xffffff, 140), HEXA(0xffffff, 0), y0, 0.5f), RAW));
    stroke(b, t, 1.0f / b->k, flag(solid(color_alpha(color_mix(hue, HEX(0), 0.4f), 0.22f)), RAW));
}

/* ---- rendering and the cache ---- */

struct cached {
    int subject, look, tile, n;
    color tint;
    unsigned used;
    uint32_t *px; /* n*n premultiplied ARGB */
};
#define CACHE_N 96
static struct cached cache[CACHE_N];
static unsigned tick;

static uint32_t *render(int subject, enum icon_look look, int tile, int n, color tint) {
    ib b = {n, calloc((size_t)n * (size_t)n * 4, sizeof(float)), look, tint, 0, 0, (float)n};
    uint32_t *out = malloc((size_t)n * (size_t)n * 4);
    if (!b.p || !out) {
        free(b.p);
        free(out);
        return NULL;
    }
    if (tile) {
        draw_tile(&b, subject, tile);
        b.ox = n * 0.16f, b.oy = n * 0.14f, b.k = n * 0.68f;
    }
    draw_subject(&b, subject);
    for (size_t i = 0; i < (size_t)n * (size_t)n; i++) {
        float *q = &b.p[i * 4];
        int a = (int)lroundf(clampf(q[3]) * 255), r = (int)lroundf(clampf(q[0]) * 255);
        int g = (int)lroundf(clampf(q[1]) * 255), bl = (int)lroundf(clampf(q[2]) * 255);
        out[i] = RGBA(r > a ? a : r, g > a ? a : g, bl > a ? a : bl, a);
    }
    free(b.p);
    return out;
}

static const uint32_t *lookup(int subject, enum icon_look look, int tile, int n, color tint) {
    if (look != LOOK_MONO)
        tint = 0; /* only mono pictures depend on it */
    struct cached *slot = NULL;
    for (int i = 0; i < CACHE_N; i++) {
        struct cached *e = &cache[i];
        if (e->px && e->subject == subject && e->look == (int)look && e->tile == tile && e->n == n && e->tint == tint) {
            e->used = ++tick;
            return e->px;
        }
        /* an empty slot, else the one unused the longest */
        if (!slot || (slot->px && (!e->px || e->used < slot->used)))
            slot = e;
    }
    free(slot->px);
    *slot = (struct cached){subject, look, tile, n, tint, ++tick, render(subject, look, tile, n, tint)};
    return slot->px;
}

/* Premultiplied picture over the (opaque) frame, faded by opacity; hover
 * lifts it a touch towards white. */
static void blit(canvas *c, const uint32_t *px, int n, int x, int y, float opacity, bool hover) {
    uint32_t o = (uint32_t)lroundf(clampf(opacity) * 256);
    if (!o)
        return;
    for (int j = 0; j < n; j++) {
        int py = y + j;
        if (py < c->cy0 || py >= c->cy1)
            continue;
        for (int i = 0; i < n; i++) {
            int qx = x + i;
            if (qx < c->cx0 || qx >= c->cx1)
                continue;
            uint32_t s = px[(size_t)j * (size_t)n + (size_t)i];
            uint32_t a = C_A(s) * o >> 8;
            if (!a)
                continue;
            uint32_t r = C_R(s) * o >> 8, g = C_G(s) * o >> 8, b = C_B(s) * o >> 8;
            if (hover)
                r += (a - r) / 10, g += (a - g) / 10, b += (a - b) / 10;
            uint32_t *d = &c->px[(size_t)py * (size_t)c->w + (size_t)qx], ia = 255 - a;
            *d = 0xff000000u | (r + C_R(*d) * ia / 255) << 16 | (g + C_G(*d) * ia / 255) << 8 | (b + C_B(*d) * ia / 255);
        }
    }
}

void icon_art(canvas *c, rect r, int subject, enum icon_look look, int tile, color tint, float opacity, bool hover) {
    int n = r.w < r.h ? r.w : r.h;
    if (n < 4 || !c)
        return;
    if (n > 512)
        n = 512;
    if (look == LOOK_LINE || look == LOOK_LEGACY)
        look = LOOK_COLOR;
    tint |= 0xff000000u; /* fading is the opacity's job, not the tint's */
    const uint32_t *px = lookup(subject, look, tile, n, tint);
    if (px)
        blit(c, px, n, r.x + (r.w - n) / 2, r.y + (r.h - n) / 2, opacity, hover);
}

/* ---- names ---- */

enum icon_look icon_look_of(const char *s) {
    if (!s || !*s || !strcasecmp(s, "color") || !strcasecmp(s, "colour") || !strcasecmp(s, "colorful"))
        return LOOK_COLOR;
    if (!strcasecmp(s, "line") || !strcasecmp(s, "outline"))
        return LOOK_LINE;
    if (!strcasecmp(s, "mono") || !strcasecmp(s, "glyph") || !strcasecmp(s, "tinted") || !strcasecmp(s, "plain"))
        return LOOK_MONO;
    if (!strcasecmp(s, "classic") || !strcasecmp(s, "glossy") || !strcasecmp(s, "xp"))
        return LOOK_CLASSIC;
    if (!strcasecmp(s, "filled") || !strcasecmp(s, "glow") || !strcasecmp(s, "duotone"))
        return LOOK_LEGACY;
    return LOOK_COLOR;
}

int icon_tile_of(const char *s) {
    if (!s || !*s)
        return 0;
    if (!strcasecmp(s, "circle"))
        return 1;
    if (!strcasecmp(s, "squircle"))
        return 2;
    if (!strcasecmp(s, "square"))
        return 3;
    return 0;
}

bool icon_is_line(int app, const char *style, const char *shape) {
    if (app < 0 || app >= APP_COUNT)
        return false;
    enum icon_look l = icon_look_of(style);
    /* the older styles without a tile always were the line symbol */
    return (l == LOOK_LINE || l == LOOK_LEGACY) && !icon_tile_of(shape);
}

bool icon_art_styled(canvas *c, rect r, int app, const char *style, const char *shape, color tint, float opacity,
                     bool hover) {
    enum icon_look l = icon_look_of(style);
    if (l == LOOK_LINE || l == LOOK_LEGACY)
        return false;
    icon_art(c, r, app, l, icon_tile_of(shape), tint, opacity, hover);
    return true;
}

int icon_subject_named(const char *name) {
    static const struct {
        const char *n;
        int s;
    } names[] = {{"folder", ICON_FOLDER},   {"directory", ICON_FOLDER},   {"document", ICON_DOCUMENT},
                 {"doc", ICON_DOCUMENT},    {"file", ICON_DOCUMENT},      {"text", ICON_DOCUMENT},
                 {"image", ICON_IMAGE},     {"photo", ICON_IMAGE},        {"picture", ICON_IMAGE},
                 {"audio", ICON_AUDIO},     {"music", ICON_AUDIO},        {"sound", ICON_AUDIO},
                 {"video", ICON_VIDEO},     {"movie", ICON_VIDEO},        {"film", ICON_VIDEO},
                 {"archive", ICON_ARCHIVE}, {"zip", ICON_ARCHIVE},        {"code", ICON_CODE},
                 {"source", ICON_CODE},     {"executable", ICON_EXECUTABLE}, {"program", ICON_EXECUTABLE},
                 {"binary", ICON_EXECUTABLE}};
    for (size_t i = 0; name && i < sizeof names / sizeof names[0]; i++)
        if (!strcasecmp(name, names[i].n))
            return names[i].s;
    return -1;
}

int icon_file_subject(const char *name, bool is_dir, bool is_exec) {
    if (is_dir)
        return ICON_FOLDER;
    const char *dot = name ? strrchr(name, '.') : NULL;
    static const struct {
        const char *ext;
        int s;
    } kinds[] = {
        {"png", ICON_IMAGE},    {"jpg", ICON_IMAGE},     {"jpeg", ICON_IMAGE},   {"gif", ICON_IMAGE},
        {"bmp", ICON_IMAGE},    {"webp", ICON_IMAGE},    {"svg", ICON_IMAGE},    {"ppm", ICON_IMAGE},
        {"xpm", ICON_IMAGE},    {"ico", ICON_IMAGE},     {"mp3", ICON_AUDIO},    {"wav", ICON_AUDIO},
        {"ogg", ICON_AUDIO},    {"flac", ICON_AUDIO},    {"m4a", ICON_AUDIO},    {"opus", ICON_AUDIO},
        {"mp4", ICON_VIDEO},    {"mkv", ICON_VIDEO},     {"webm", ICON_VIDEO},   {"mov", ICON_VIDEO},
        {"avi", ICON_VIDEO},    {"zip", ICON_ARCHIVE},   {"tar", ICON_ARCHIVE},  {"gz", ICON_ARCHIVE},
        {"xz", ICON_ARCHIVE},   {"bz2", ICON_ARCHIVE},   {"zst", ICON_ARCHIVE},  {"7z", ICON_ARCHIVE},
        {"cpio", ICON_ARCHIVE}, {"deb", ICON_ARCHIVE},   {"c", ICON_CODE},       {"h", ICON_CODE},
        {"py", ICON_CODE},      {"js", ICON_CODE},       {"ts", ICON_CODE},      {"lua", ICON_CODE},
        {"sh", ICON_CODE},      {"rs", ICON_CODE},       {"go", ICON_CODE},      {"cpp", ICON_CODE},
        {"json", ICON_CODE},    {"xml", ICON_CODE},      {"html", ICON_CODE},    {"css", ICON_CODE},
        {"mjs", ICON_CODE},     {"java", ICON_CODE},     {"so", ICON_EXECUTABLE}, {"appimage", ICON_EXECUTABLE},
        {"bin", ICON_EXECUTABLE}, {"elf", ICON_EXECUTABLE}, {"exe", ICON_EXECUTABLE},
    };
    if (dot && dot != name)
        for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
            if (!strcasecmp(dot + 1, kinds[i].ext))
                return kinds[i].s;
    return is_exec ? ICON_EXECUTABLE : ICON_DOCUMENT;
}
