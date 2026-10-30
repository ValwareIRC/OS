/* The face, drawn the way anime faces are: flat shapes and lines on skin.
 * Big eyes: a white with the lid's shadow across its top, a large iris
 * tucked under the upper lid (darker at the top and rim, a lighter pool in
 * its lower half, a pupil), highlights that sit on the same side in both
 * eyes, a thick lash band that thickens towards the outer corner and ends in
 * a wing, the crease above, a fine lower lash. Small brows, a nose mark, a
 * small mouth whose shapes follow speech and mood, blush. Every expression
 * is a variant drawn in the character's own style, so a happy ^ ^ or a pout
 * still looks like her.
 *
 * The texture covers the head's full width and height (see head_uv). Eyes
 * are laid out in "eye units" (0 at the inner corner, 1 at the outer, y up),
 * mapped to pixels, mirrored for the other eye. */
#include <stdlib.h>
#include <string.h>

#include "../shell/gfx.h"
#include "face.h"

typedef struct {
    float x, y;
} p2;

face_params face_defaults(void) {
    return (face_params){.skin = 0xfbe4d8, .iris_top = 0x2a3a8c, .iris_bot = 0x58b8f0, .white = 0xfdfcfc,
                         .lash = 0x3a2832, .brow = 0x6a4a4e, .lip = 0xc0646e, .blush_col = 0xff8f9c, .pupil_col = 0x161634,
                         .eye_shape = EYE_ROUND, .pupil = PUPIL_ROUND, .highlights = 2, .lashes = 1, .brow_style = BROW_SOFT,
                         .mouth_style = MOUTH_SMALL, .nose = NOSE_DOT, .eye_size = 1.0f, .eye_gap = 1.0f, .eye_tilt = 0,
                         .blush = 0.4f};
}

static inline color C(rgb c, float a) { return (color)((uint32_t)(clampf(a, 0, 1) * 255) << 24 | (c & 0xffffff)); }

static p2 bez(p2 a, p2 b, p2 c, p2 d, float t) {
    float u = 1 - t;
    float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    return (p2){a.x * w0 + b.x * w1 + c.x * w2 + d.x * w3, a.y * w0 + b.y * w1 + c.y * w2 + d.y * w3};
}

/* A filled shape from points. */
static void poly(canvas *cv, const p2 *p, int n, color col) {
    if (n < 3)
        return;
    float *xy = malloc(sizeof(float) * 2 * n);
    if (!xy)
        return;
    for (int i = 0; i < n; i++)
        xy[i * 2] = p[i].x, xy[i * 2 + 1] = p[i].y;
    fill_polygon(cv, xy, n, col);
    free(xy);
}

/* A line along points, its width changing along it, round at the ends. */
static void stroke(canvas *cv, const p2 *p, const float *w, int n, color col) {
    if (n < 2)
        return;
    p2 *outl = malloc(sizeof(p2) * 2 * n);
    if (!outl)
        return;
    for (int i = 0; i < n; i++) {
        p2 d = i == 0 ? (p2){p[1].x - p[0].x, p[1].y - p[0].y}
               : i == n - 1 ? (p2){p[n - 1].x - p[n - 2].x, p[n - 1].y - p[n - 2].y}
                            : (p2){p[i + 1].x - p[i - 1].x, p[i + 1].y - p[i - 1].y};
        float l = sqrtf(d.x * d.x + d.y * d.y);
        if (l < 1e-6f)
            l = 1;
        p2 nrm = {-d.y / l, d.x / l};
        outl[i] = (p2){p[i].x + nrm.x * w[i] * 0.5f, p[i].y + nrm.y * w[i] * 0.5f};
        outl[2 * n - 1 - i] = (p2){p[i].x - nrm.x * w[i] * 0.5f, p[i].y - nrm.y * w[i] * 0.5f};
    }
    poly(cv, outl, 2 * n, col);
    if (w[0] > 1.5f)
        fill_circle(cv, p[0].x, p[0].y, w[0] * 0.5f, col);
    if (w[n - 1] > 1.5f)
        fill_circle(cv, p[n - 1].x, p[n - 1].y, w[n - 1] * 0.5f, col);
    free(outl);
}

/* A soft-edged ellipse (blush, glows): `a` at the centre, nothing at the edge. */
static void soft_ellipse(canvas *cv, float cx, float cy, float rx, float ry, rgb col, float a) {
    int x0 = (int)(cx - rx), x1 = (int)(cx + rx) + 1, y0 = (int)(cy - ry), y1 = (int)(cy + ry) + 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
                continue;
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry, d = dx * dx + dy * dy;
            if (d >= 1)
                continue;
            float k = (1 - d) * (1 - d) * a;
            uint32_t *p = &cv->px[y * cv->w + x];
            *p = 0xff000000u | rgb_mix(*p & 0xffffff, col, k);
        }
}

/* ---- a coverage mask: which pixels a shape covers, 0..255 ---- */

typedef struct {
    int x0, y0, w, h;
    uint8_t *a;
} mask;

static void mask_poly(mask *mk, const p2 *p, int n) {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        minx = fminf(minx, p[i].x), maxx = fmaxf(maxx, p[i].x);
        miny = fminf(miny, p[i].y), maxy = fmaxf(maxy, p[i].y);
    }
    mk->x0 = (int)minx - 2, mk->y0 = (int)miny - 2;
    mk->w = (int)(maxx - minx) + 5, mk->h = (int)(maxy - miny) + 5;
    canvas *c = canvas_new(mk->w, mk->h);
    mk->a = calloc((size_t)mk->w * mk->h, 1);
    p2 *q = malloc(sizeof(p2) * n);
    if (!c || !mk->a || !q) {
        canvas_free(c);
        free(q);
        return;
    }
    for (int i = 0; i < mk->w * mk->h; i++)
        c->px[i] = 0xff000000u;
    for (int i = 0; i < n; i++)
        q[i] = (p2){p[i].x - mk->x0, p[i].y - mk->y0};
    poly(c, q, n, 0xffffffffu);
    for (int i = 0; i < mk->w * mk->h; i++)
        mk->a[i] = (uint8_t)((c->px[i] >> 16) & 255);
    free(q);
    canvas_free(c);
}

static inline float mask_at(const mask *mk, int x, int y) {
    int mx = x - mk->x0, my = y - mk->y0;
    if (!mk->a || mx < 0 || my < 0 || mx >= mk->w || my >= mk->h)
        return 0;
    return mk->a[my * mk->w + mx] / 255.0f;
}

static inline void blend_px(canvas *cv, int x, int y, rgb c, float a) {
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h || a <= 0)
        return;
    uint32_t *p = &cv->px[y * cv->w + x];
    *p = 0xff000000u | rgb_mix(*p & 0xffffff, c, a);
}

/* ---- the eyes ---- */

typedef struct {
    p2 o;       /* inner corner, pixels */
    float E;    /* eye width, pixels */
    float side; /* +1 for the eye on the picture's right, -1 mirrored */
} eyeframe;

static inline p2 EP(const eyeframe *f, float ex, float ey) { return (p2){f->o.x + f->side * ex * f->E, f->o.y - ey * f->E}; }

typedef struct {
    float up, outer, low; /* how high the upper lid arches, where the outer corner sits, how low the lower lid dips */
} lidshape;

static lidshape lid_for(const face_params *fp) {
    lidshape s;
    switch (fp->eye_shape) {
    case EYE_ALMOND: s = (lidshape){0.78f, 0.16f, 0.16f}; break;
    case EYE_DROOPY: s = (lidshape){0.86f, -0.06f, 0.2f}; break;
    case EYE_SHARP: s = (lidshape){0.66f, 0.26f, 0.12f}; break;
    case EYE_SLEEPY: s = (lidshape){0.62f, 0.04f, 0.2f}; break;
    default: s = (lidshape){0.96f, 0.12f, 0.24f}; break;
    }
    s.outer += fp->eye_tilt * 0.12f;
    return s;
}

#define NCURVE 28

/* The upper and lower lid curves, eye units. `lower_lid`: how far the upper
 * lid has come down (0 open, 1 shut); `angry` flattens it towards the nose,
 * `sad` lifts it there. */
static void lids(lidshape s, float lower_lid, float angry, float sad, p2 *up, p2 *lo) {
    float h = s.up * (1 - lower_lid);
    p2 I = {0, 0.02f}, O = {1, s.outer};
    p2 u1 = {0.04f, h * (1 - angry * 0.6f) + sad * 0.1f}, u2 = {0.6f, h * (1.08f - sad * 0.3f) + s.outer * 0.3f};
    p2 l1 = {0.3f, -s.low}, l2 = {0.82f, -s.low * 0.85f + s.outer * 0.45f};
    for (int i = 0; i < NCURVE; i++) {
        float t = (float)i / (NCURVE - 1);
        up[i] = bez(I, u1, u2, O, t);
        lo[i] = bez(I, l1, l2, O, t);
        if (up[i].y < lo[i].y)
            up[i].y = lo[i].y;
    }
}

/* the lash band along the upper lid: thin at the inner corner, thick in the
 * outer half, ending in a wing past the corner */
static void lash_band(canvas *cv, const eyeframe *f, const p2 *up, const face_params *fp, float thick) {
    p2 pts[NCURVE * 2 + 2];
    int n = 0;
    int i0 = 1;
    for (int i = i0; i < NCURVE; i++) /* the lower edge: the lid line */
        pts[n++] = EP(f, up[i].x, up[i].y - 0.012f);
    p2 O = up[NCURVE - 1], P = up[NCURVE - 4];
    float dx = O.x - P.x, dy = O.y - P.y, l = sqrtf(dx * dx + dy * dy);
    dx /= l, dy /= l;
    pts[n++] = EP(f, O.x + dx * 0.2f + 0.02f, O.y + dy * 0.2f - 0.035f); /* the wing's tip */
    for (int i = NCURVE - 1; i >= i0; i--) {                            /* the upper edge */
        float t = (float)i / (NCURVE - 1);
        float w = thick * (0.05f + 0.11f * smoothf(0.0f, 0.75f, t)) * (1 - 0.35f * smoothf(0.85f, 1.0f, t));
        float ddx, ddy;
        if (i == 0 || i == NCURVE - 1)
            ddx = up[i].x - up[i > 0 ? i - 1 : 0].x, ddy = up[i].y - up[i > 0 ? i - 1 : 0].y;
        else
            ddx = up[i + 1].x - up[i - 1].x, ddy = up[i + 1].y - up[i - 1].y;
        float ll = sqrtf(ddx * ddx + ddy * ddy);
        if (ll < 1e-6f)
            ll = 1;
        pts[n++] = EP(f, up[i].x - ddy / ll * w, up[i].y + ddx / ll * w);
    }
    poly(cv, pts, n, C(fp->lash, 1));
    /* separate lashes at the outer corner */
    int extra = fp->lashes >= 2 ? 3 : fp->lashes >= 1 ? 1 : 0;
    for (int k = 0; k < extra; k++) {
        float t = 0.93f - k * 0.1f;
        p2 b = up[(int)(t * (NCURVE - 1))];
        p2 sp[3] = {EP(f, b.x - 0.05f, b.y + 0.08f), EP(f, b.x + 0.1f + k * 0.02f, b.y + 0.2f - k * 0.02f),
                    EP(f, b.x + 0.03f, b.y + 0.1f)};
        poly(cv, sp, 3, C(fp->lash, 1));
    }
}

/* the crease above the eye */
static void crease(canvas *cv, const eyeframe *f, const p2 *up, const face_params *fp, float lift) {
    p2 pts[NCURVE];
    float w[NCURVE];
    int n = 0;
    for (int i = NCURVE / 4; i < NCURVE - 2; i++, n++) {
        float t = (float)(i - NCURVE / 4) / (NCURVE - 2 - NCURVE / 4);
        pts[n] = EP(f, up[i].x + 0.03f, up[i].y + lift + 0.03f * sinf(PI_F * t));
        w[n] = f->E * 0.022f * sinf(PI_F * t) + 0.5f;
    }
    stroke(cv, pts, w, n, C(rgb_mix(fp->lash, fp->skin, 0.45f), 0.8f));
}

static void closed_line(canvas *cv, const eyeframe *f, const face_params *fp, lidshape s, bool happy) {
    p2 pts[NCURVE];
    float w[NCURVE];
    p2 I = {0.02f, 0.06f}, O = {1.02f, s.outer * 0.6f + 0.02f};
    p2 c1 = happy ? (p2){0.22f, 0.5f} : (p2){0.3f, -0.18f};
    p2 c2 = happy ? (p2){0.74f, 0.52f} : (p2){0.74f, -0.16f};
    for (int i = 0; i < NCURVE; i++) {
        float t = (float)i / (NCURVE - 1);
        p2 q = bez(I, c1, c2, O, t);
        pts[i] = EP(f, q.x, q.y);
        w[i] = f->E * (0.05f + 0.05f * sinf(PI_F * t));
    }
    stroke(cv, pts, w, NCURVE, C(fp->lash, 1));
    if (!happy && fp->lashes) { /* little lashes on a closed eye */
        p2 b = {0.9f, s.outer * 0.45f - 0.05f};
        p2 sp[3] = {EP(f, b.x - 0.06f, b.y + 0.02f), EP(f, b.x + 0.12f, b.y - 0.12f), EP(f, b.x + 0.04f, b.y + 0.05f)};
        poly(cv, sp, 3, C(fp->lash, 1));
    }
}

/* The iris itself depends only on the look, not on where she looks or how
 * she feels: drawn once into a small picture (colour, and coverage in the
 * alpha) and placed wherever the eye needs it. One per pupil kind and size. */
typedef struct {
    int w, h, prx, pry, kind;
    rgb top, bot, pupil;
    uint32_t *px;
} iris_pic;

static iris_pic iris_cache[4];

static const iris_pic *iris_get(const face_params *fp, float prx, float pry, int kind) {
    int irx = (int)(prx + 0.5f), iry = (int)(pry + 0.5f);
    for (int i = 0; i < 4; i++) {
        iris_pic *ip = &iris_cache[i];
        if (ip->px && ip->prx == irx && ip->pry == iry && ip->kind == kind && ip->top == fp->iris_top &&
            ip->bot == fp->iris_bot && ip->pupil == fp->pupil_col)
            return ip;
    }
    /* the least recently made goes */
    static int next;
    iris_pic *ip = &iris_cache[next];
    next = (next + 1) % 4;
    free(ip->px);
    ip->prx = irx, ip->pry = iry, ip->kind = kind, ip->top = fp->iris_top, ip->bot = fp->iris_bot, ip->pupil = fp->pupil_col;
    ip->w = irx * 2 + 4, ip->h = iry * 2 + 4;
    ip->px = calloc((size_t)ip->w * ip->h, 4);
    if (!ip->px)
        return NULL;
    rgb rim = rgb_mix(rgb_scale(fp->iris_top, 0.55f), 0x000010, 0.2f);
    rgb pool = rgb_mix(fp->iris_bot, 0xffffff, 0.28f);
    float cx = ip->w * 0.5f, cy = ip->h * 0.5f;
    for (int y = 0; y < ip->h; y++)
        for (int x = 0; x < ip->w; x++) {
            float dx = (x + 0.5f - cx) / prx, dy = (y + 0.5f - cy) / pry;
            float r = sqrtf(dx * dx + dy * dy);
            float cov = clampf((1 - r) * prx * 0.9f, 0, 1);
            if (cov <= 0)
                continue;
            /* top dark, bottom light */
            float t = clampf((dy + 1) * 0.5f, 0, 1);
            rgb col = rgb_mix(fp->iris_top, fp->iris_bot, smoothf(0.15f, 0.95f, t));
            /* a lighter pool in the lower half */
            float px = dx / 0.72f, py = (dy - 0.42f) / 0.46f;
            float pd = px * px + py * py;
            col = rgb_mix(col, pool, (1 - smoothf(0.55f, 1.0f, pd)) * 0.75f);
            /* fine rays */
            float a = atan2f(dy, dx);
            col = rgb_mix(col, rgb_scale(col, 0.82f), 0.25f * smoothf(0.3f, 0.8f, r) * (0.5f + 0.5f * sinf(a * 23)));
            /* the rim */
            col = rgb_mix(col, rim, smoothf(0.8f, 0.97f, r));
            /* the pupil */
            float pdist;
            switch (kind) {
            case PUPIL_SLIT: pdist = (dx * dx) / (0.12f * 0.12f) + (dy * dy) / (0.62f * 0.62f); break;
            case PUPIL_STAR: {
                float rr = sqrtf(dx * dx + dy * dy);
                pdist = rr / (0.38f * (0.55f + 0.45f * cosf(5 * (a + PI_F / 2))));
                break;
            }
            case PUPIL_HEART: {
                float hx = dx / 0.44f, hy = -(dy - 0.02f) / 0.44f;
                float q = hx * hx + hy * hy - 1;
                pdist = (q * q * q - hx * hx * hy * hy * hy) < 0 ? 0.5f : 2;
                break;
            }
            case PUPIL_RING: {
                float e = sqrtf(dx * dx / (0.22f * 0.22f) + dy * dy / (0.46f * 0.46f));
                pdist = fabsf(e - 1) < 0.22f ? 0.5f : 2;
                break;
            }
            default: pdist = (dx * dx) / (0.3f * 0.3f) + (dy * dy) / (0.36f * 0.36f); break;
            }
            if (pdist < 1)
                col = rgb_mix(col, kind == PUPIL_HEART ? 0xe0306a : fp->pupil_col, kind == PUPIL_RING ? 0.85f : 0.9f);
            ip->px[y * ip->w + x] = (uint32_t)(cov * 255 + 0.5f) << 24 | col;
        }
    return ip;
}

/* the iris, clipped to the eye's opening */
static void iris(canvas *cv, const eyeframe *f, const mask *open, const face_params *fp, float cx, float cy, float rx,
                 float ry, int kind, const p2 *up) {
    p2 c = EP(f, cx, cy);
    float prx = rx * f->E, pry = ry * f->E;
    const iris_pic *ip = iris_get(fp, prx, pry, kind);
    if (ip) {
        int ox = (int)(c.x - ip->w * 0.5f + 0.5f), oy = (int)(c.y - ip->h * 0.5f + 0.5f);
        for (int y = 0; y < ip->h; y++)
            for (int x = 0; x < ip->w; x++) {
                uint32_t p = ip->px[y * ip->w + x];
                if (!(p >> 24))
                    continue;
                float m = mask_at(open, ox + x, oy + y);
                if (m > 0)
                    blend_px(cv, ox + x, oy + y, p & 0xffffff, m * (p >> 24) / 255.0f);
            }
    }
    /* the upper lid's shadow over the iris and the white */
    for (int y = open->y0; y < open->y0 + open->h; y++)
        for (int x = open->x0; x < open->x0 + open->w; x++) {
            float m = mask_at(open, x, y);
            if (m <= 0)
                continue;
            float ex = f->side * ((x + 0.5f) - f->o.x) / f->E;
            int i = (int)clampf(ex * (NCURVE - 1), 0, NCURVE - 1);
            float lid = f->o.y - up[i].y * f->E;
            float dd = ((y + 0.5f) - lid) / f->E;
            float k = clampf(1 - dd / 0.3f, 0, 1);
            if (k > 0)
                blend_px(cv, x, y, 0x2a2440, m * k * k * 0.42f);
        }
    /* highlights: a big one up and to the picture's left, a small one low
     * on the right, the same in both eyes (the light is in one place) */
    static const float hl[3][5] = {{-0.4f, -0.46f, 0.3f, 0.2f, -0.6f}, {0.42f, 0.48f, 0.13f, 0.11f, 0}, {0.2f, -0.6f, 0.08f, 0.07f, 0}};
    for (int k = 0; k < fp->highlights && k < 3; k++) {
        float hx = c.x + hl[k][0] * prx, hy = c.y + hl[k][1] * pry, hrx = hl[k][2] * prx * 1.15f, hry = hl[k][3] * prx * 1.3f;
        float ca = cosf(hl[k][4]), sa = sinf(hl[k][4]);
        int R = (int)(fmaxf(hrx, hry) + 2);
        for (int y = (int)hy - R; y <= (int)hy + R; y++)
            for (int x = (int)hx - R; x <= (int)hx + R; x++) {
                float m = mask_at(open, x, y);
                if (m <= 0)
                    continue;
                float ux = x + 0.5f - hx, uy = y + 0.5f - hy;
                float rx2 = (ux * ca + uy * sa) / hrx, ry2 = (-ux * sa + uy * ca) / hry;
                float cov = clampf((1 - sqrtf(rx2 * rx2 + ry2 * ry2)) * hry * 0.9f, 0, 1);
                blend_px(cv, x, y, 0xffffff, m * cov);
            }
    }
}

static void sparkle(canvas *cv, float x, float y, float r) {
    p2 s[8];
    for (int i = 0; i < 8; i++) {
        float a = i * PI_F / 4, rr = (i & 1) ? r * 0.22f : r;
        s[i] = (p2){x + cosf(a) * rr, y + sinf(a) * rr};
    }
    poly(cv, s, 8, 0xffffffffu);
}

static void draw_eye(canvas *cv, const eyeframe *f, const face_params *fp, const face_state *fs, int state) {
    lidshape s = lid_for(fp);
    if (state == FE_CLOSED || state == FE_HAPPY) {
        closed_line(cv, f, fp, s, state == FE_HAPPY);
        return;
    }
    if (state == FE_DIZZY) {
        p2 pts[48];
        float w[48];
        for (int i = 0; i < 48; i++) {
            float t = i / 47.0f, a = t * 4 * PI_F, r = 0.05f + 0.36f * t;
            pts[i] = EP(f, 0.5f + cosf(a) * r, s.up * 0.38f + sinf(a) * r * 0.9f);
            w[i] = f->E * 0.04f;
        }
        stroke(cv, pts, w, 48, C(fp->lash, 1));
        return;
    }
    float lower = state == FE_HALF ? 0.5f : 0, angry = state == FE_ANGRY ? 1 : 0, sad = state == FE_SAD ? 1 : 0;
    if (state == FE_WIDE)
        s.up *= 1.1f, s.low *= 1.15f;
    p2 up[NCURVE], lo[NCURVE];
    lids(s, lower, angry, sad, up, lo);
    p2 open[NCURVE * 2];
    for (int i = 0; i < NCURVE; i++)
        open[i] = EP(f, up[i].x, up[i].y), open[NCURVE * 2 - 1 - i] = EP(f, lo[i].x, lo[i].y);
    mask mk = {0};
    mask_poly(&mk, open, NCURVE * 2);
    poly(cv, open, NCURVE * 2, C(fp->white, 1));
    float scale = state == FE_WIDE ? 0.8f : 1;
    float cx = 0.5f + f->side * fs->gx * 0.14f, cy = s.up * 0.36f + fs->gy * 0.1f - lower * 0.12f;
    int kind = state == FE_HEART ? PUPIL_HEART : fp->pupil;
    iris(cv, f, &mk, fp, cx, cy, 0.34f * scale, 0.47f * scale, kind, up);
    if (state == FE_SPARKLE) {
        p2 c = EP(f, cx - 0.08f, cy + 0.12f);
        sparkle(cv, c.x, c.y, f->E * 0.17f);
        p2 c2 = EP(f, cx + 0.13f, cy - 0.18f);
        sparkle(cv, c2.x, c2.y, f->E * 0.09f);
    }
    free(mk.a);
    lash_band(cv, f, up, fp, state == FE_ANGRY ? 1.2f : 1);
    if (state != FE_HALF)
        crease(cv, f, up, fp, 0.13f);
    /* the lower lash, on the outer half */
    p2 lp[14];
    float lw[14];
    for (int i = 0; i < 14; i++) {
        float t = 0.45f + 0.55f * i / 13.0f;
        p2 q = lo[(int)(t * (NCURVE - 1))];
        lp[i] = EP(f, q.x, q.y - 0.012f);
        lw[i] = f->E * 0.026f * sinf(PI_F * (i / 13.0f)) + 0.6f;
    }
    stroke(cv, lp, lw, 14, C(fp->lash, 0.75f));
    if (state == FE_TEARS) {
        p2 q = lo[NCURVE / 3];
        p2 c = EP(f, q.x, q.y - 0.1f);
        soft_ellipse(cv, c.x, c.y + f->E * 0.15f, f->E * 0.07f, f->E * 0.18f, 0x9fd8ff, 0.9f);
        fill_circle(cv, c.x, c.y + f->E * 0.3f, f->E * 0.075f, C(0xbfe6ff, 0.95f));
    }
}

static void draw_brow(canvas *cv, const eyeframe *f, const face_params *fp, int state, lidshape s) {
    float in = 0, mid = 0, out = 0;
    switch (state) {
    case FB_RAISED: in = 0.12f, mid = 0.16f, out = 0.1f; break;
    case FB_FURROW: in = -0.14f, mid = -0.02f, out = 0.06f; break;
    case FB_WORRY: in = 0.15f, mid = 0.05f, out = -0.07f; break;
    }
    float base = s.up + 0.36f;
    float arch = fp->brow_style == BROW_ARCHED ? 0.1f : fp->brow_style == BROW_STRAIGHT ? 0.0f : 0.05f;
    float thick = fp->brow_style == BROW_THICK ? 1.8f : fp->brow_style == BROW_ARCHED ? 0.75f : 1;
    float len = fp->brow_style == BROW_SHORT ? 0.62f : 1;
    p2 a = {0.0f, base + in}, b = {0.35f, base + arch + mid + 0.06f}, c = {0.7f * len, base + arch + mid + 0.05f},
       d = {1.05f * len, base - 0.08f + out};
    p2 pts[16];
    float w[16];
    for (int i = 0; i < 16; i++) {
        float t = i / 15.0f;
        p2 q = bez(a, b, c, d, t);
        pts[i] = EP(f, q.x, q.y);
        w[i] = f->E * thick * (0.045f - 0.025f * t);
    }
    stroke(cv, pts, w, 16, C(fp->brow, 1));
}

/* ---- the mouth ---- */

static void mouth_shape(canvas *cv, float cx, float cy, float M, const face_params *fp, const face_state *fs) {
    color line = C(fp->lip, 1);
    rgb inside = 0x7a2a38, tongue = 0xe98a92;
    float o = clampf(fs->open, 0.3f, 1);
    float ax = 0, ay = 0; /* an open mouth's half width and height, in M */
    switch (fs->mouth) {
    case FM_A: ax = 0.2f, ay = 0.26f * o + 0.03f; break;
    case FM_I: ax = 0.3f, ay = 0.08f * o + 0.03f; break;
    case FM_U: ax = 0.11f, ay = 0.11f * o + 0.04f; break;
    case FM_E: ax = 0.26f, ay = 0.15f * o + 0.03f; break;
    case FM_O: ax = 0.16f, ay = 0.2f * o + 0.04f; break;
    case FM_SMALL_O: ax = 0.075f, ay = 0.095f; break;
    }
    if (ax > 0) {
        p2 pts[40];
        for (int i = 0; i < 40; i++) {
            float a = i * 2 * PI_F / 40;
            /* flatter on top for A and E, a rounder bottom */
            float k = (fs->mouth == FM_A || fs->mouth == FM_E) && sinf(a) < 0 ? 0.7f : 1.0f;
            pts[i] = (p2){cx + cosf(a) * ax * M, cy + sinf(a) * ay * M * k};
        }
        poly(cv, pts, 40, C(inside, 1));
        if (fs->mouth == FM_A || fs->mouth == FM_O || fs->mouth == FM_E)
            soft_ellipse(cv, cx, cy + ay * M * 0.62f, ax * M * 0.72f, ay * M * 0.42f, tongue, 0.95f);
        if (fs->mouth == FM_E || fs->mouth == FM_I) /* a glimpse of the top teeth */
            for (int x = (int)(cx - ax * M * 0.85f); x < (int)(cx + ax * M * 0.85f); x++)
                for (int y = (int)(cy - ay * M * 0.9f); y < (int)(cy - ay * M * 0.45f); y++)
                    blend_px(cv, x, y, 0xffffff, 0.92f);
        float w[41];
        p2 ring[41];
        for (int i = 0; i <= 40; i++)
            ring[i] = pts[i % 40], w[i] = fmaxf(1.2f, M * 0.03f);
        stroke(cv, ring, w, 41, line);
        return;
    }
    p2 a, b, c, d;
    switch (fs->mouth) {
    case FM_OPEN_SMILE: {
        p2 pts[26];
        int n = 0;
        for (int i = 0; i <= 12; i++) { /* the top: nearly straight */
            float t = i / 12.0f;
            pts[n++] = (p2){cx + (t - 0.5f) * 0.62f * M, cy - 0.04f * M + 0.03f * M * sinf(PI_F * t)};
        }
        for (int i = 12; i >= 0; i--) { /* the bottom: a deep curve */
            float t = i / 12.0f;
            pts[n++] = (p2){cx + (t - 0.5f) * 0.62f * M, cy - 0.04f * M + 0.34f * M * powf(sinf(PI_F * t), 0.8f)};
        }
        poly(cv, pts, n, C(inside, 1));
        soft_ellipse(cv, cx, cy + 0.22f * M, 0.2f * M, 0.09f * M, tongue, 0.95f);
        float w[13];
        for (int i = 0; i < 13; i++)
            w[i] = fmaxf(1.2f, M * 0.035f);
        stroke(cv, pts, w, 13, line);
        if (fp->mouth_style == MOUTH_FANG) {
            p2 fg[3] = {{cx + 0.14f * M, cy - 0.03f * M}, {cx + 0.22f * M, cy - 0.03f * M}, {cx + 0.18f * M, cy + 0.07f * M}};
            poly(cv, fg, 3, 0xffffffffu);
        }
        return;
    }
    case FM_SMILE: a = (p2){-0.3f, 0.07f}, b = (p2){-0.12f, -0.07f}, c = (p2){0.12f, -0.07f}, d = (p2){0.3f, 0.07f}; break;
    case FM_FROWN: a = (p2){-0.22f, -0.05f}, b = (p2){-0.08f, 0.05f}, c = (p2){0.08f, 0.05f}, d = (p2){0.22f, -0.05f}; break;
    case FM_CAT: {
        p2 pts[25];
        float w[25];
        for (int i = 0; i < 25; i++) {
            float t = i / 24.0f, x = (t - 0.5f) * 0.46f;
            float y = 0.04f * cosf(t * 4 * PI_F) - 0.02f;
            pts[i] = (p2){cx + x * M, cy - y * M};
            w[i] = fmaxf(1.2f, M * 0.03f);
        }
        stroke(cv, pts, w, 25, line);
        return;
    }
    case FM_GRIT: {
        p2 r[4] = {{cx - 0.26f * M, cy - 0.08f * M}, {cx + 0.26f * M, cy - 0.08f * M}, {cx + 0.23f * M, cy + 0.08f * M},
                   {cx - 0.23f * M, cy + 0.08f * M}};
        poly(cv, r, 4, 0xfff4f0f0u);
        p2 ln[5] = {r[0], r[1], r[2], r[3], r[0]};
        float w[5] = {M * 0.03f, M * 0.03f, M * 0.03f, M * 0.03f, M * 0.03f};
        stroke(cv, ln, w, 5, line);
        p2 mid[2] = {{cx - 0.24f * M, cy}, {cx + 0.24f * M, cy}};
        float mw[2] = {M * 0.018f, M * 0.018f};
        stroke(cv, mid, mw, 2, line);
        return;
    }
    default: a = (p2){-0.17f, 0.025f}, b = (p2){-0.06f, -0.025f}, c = (p2){0.06f, -0.025f}, d = (p2){0.17f, 0.025f}; break;
    }
    if (fp->mouth_style == MOUTH_CAT && fs->mouth == FM_CLOSED) {
        face_state cat = *fs;
        cat.mouth = FM_CAT;
        mouth_shape(cv, cx, cy, M * 0.8f, fp, &cat);
        return;
    }
    p2 pts[16];
    float w[16];
    for (int i = 0; i < 16; i++) {
        float t = i / 15.0f;
        p2 q = bez(a, b, c, d, t);
        pts[i] = (p2){cx + q.x * M, cy - q.y * M};
        w[i] = fmaxf(1.2f, M * (0.02f + 0.02f * sinf(PI_F * t)));
    }
    stroke(cv, pts, w, 16, line);
    if (fp->mouth_style == MOUTH_FANG && fs->mouth == FM_SMILE) {
        p2 fg[3] = {{cx + 0.1f * M, cy + 0.02f * M}, {cx + 0.17f * M, cy + 0.0f * M}, {cx + 0.14f * M, cy + 0.08f * M}};
        poly(cv, fg, 3, 0xffffffffu);
    }
}

void face_draw(ftex *t, const head_params *hp, const face_params *fp, const face_state *fs) {
    canvas cv = {.w = t->w, .h = t->h, .px = t->px};
    canvas_unclip(&cv);
    for (int i = 0; i < t->w * t->h; i++)
        t->px[i] = 0xff000000u | fp->skin;
    float TW = (float)t->w, TH = (float)t->h;
    float cx = TW * 0.5f;
    float eye_y = TH * (0.5f - hp->eye_y);
    float E = fp->eye_size * 0.235f * TW;
    float gap = fp->eye_gap * 0.078f * TW;
    lidshape s = lid_for(fp);

    /* blush, under everything else */
    float b = clampf(fp->blush + fs->blush, 0, 1.4f);
    if (b > 0.02f) {
        for (int k = -1; k <= 1; k += 2) {
            float bx = cx + k * (gap + E * 0.62f), by = eye_y + E * 0.5f;
            soft_ellipse(&cv, bx, by, E * 0.6f, E * 0.3f, fp->blush_col, 0.4f * b);
            if (b > 0.75f)
                for (int i = 0; i < 3; i++) {
                    p2 hl[2] = {{bx - E * 0.18f + i * E * 0.14f, by - E * 0.07f}, {bx - E * 0.25f + i * E * 0.14f, by + E * 0.09f}};
                    float w[2] = {E * 0.022f, E * 0.022f};
                    stroke(&cv, hl, w, 2, C(rgb_scale(fp->blush_col, 0.85f), 0.8f));
                }
        }
    }
    /* the nose */
    float ny = eye_y + TH * 0.115f;
    if (fp->nose == NOSE_LINE) {
        p2 nl[2] = {{cx + E * 0.03f, ny - E * 0.08f}, {cx - E * 0.01f, ny}};
        float w[2] = {E * 0.025f, E * 0.02f};
        stroke(&cv, nl, w, 2, C(rgb_scale(fp->skin, 0.72f), 0.9f));
    } else if (fp->nose == NOSE_DOT) {
        soft_ellipse(&cv, cx + E * 0.015f, ny, E * 0.04f, E * 0.028f, rgb_scale(fp->skin, 0.72f), 0.8f);
    }
    /* the mouth */
    mouth_shape(&cv, cx, eye_y + TH * 0.235f, TW * 0.2f, fp, fs);
    /* eyes and brows */
    for (int k = -1; k <= 1; k += 2) {
        eyeframe f = {{cx + k * gap, eye_y}, E, (float)k};
        int state = fs->eyes;
        if (state == FE_WINK_L)
            state = k > 0 ? FE_HAPPY : FE_OPEN;
        else if (state == FE_WINK_R)
            state = k < 0 ? FE_HAPPY : FE_OPEN;
        draw_eye(&cv, &f, fp, fs, state);
        draw_brow(&cv, &f, fp, fs->brows, s);
    }
    if (fp->mole)
        fill_circle(&cv, cx + gap + E * 0.85f, eye_y + E * 0.42f, E * 0.025f, C(0x6a4040, 1));
}
