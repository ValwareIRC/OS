#include "gfx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

canvas *canvas_new(int w, int h) {
    canvas *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->px = calloc((size_t)w * (size_t)h, 4);
    if (!c->px) {
        free(c);
        return NULL;
    }
    c->w = w;
    c->h = h;
    canvas_unclip(c);
    return c;
}

void canvas_free(canvas *c) {
    if (c) {
        free(c->px);
        free(c);
    }
}

void canvas_unclip(canvas *c) {
    c->cx0 = 0, c->cy0 = 0, c->cx1 = c->w, c->cy1 = c->h;
}

void canvas_clip(canvas *c, rect r) {
    c->cx0 = r.x < 0 ? 0 : r.x;
    c->cy0 = r.y < 0 ? 0 : r.y;
    c->cx1 = r.x + r.w > c->w ? c->w : r.x + r.w;
    c->cy1 = r.y + r.h > c->h ? c->h : r.y + r.h;
}

static inline uint32_t blend(uint32_t dst, color src, uint32_t cov /* 0..255 */) {
    uint32_t a = (C_A(src) * cov + 127) / 255;
    if (a == 0)
        return dst;
    if (a == 255)
        return src | 0xff000000u;
    uint32_t ia = 255 - a;
    uint32_t r = (C_R(src) * a + C_R(dst) * ia + 127) / 255;
    uint32_t g = (C_G(src) * a + C_G(dst) * ia + 127) / 255;
    uint32_t b = (C_B(src) * a + C_B(dst) * ia + 127) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static inline void plot(canvas *c, int x, int y, color col, uint32_t cov) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1)
        return;
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    *p = blend(*p, col, cov);
}

void canvas_blit(canvas *dst, const canvas *src, int x, int y) {
    for (int j = 0; j < src->h; j++) {
        int dy = y + j;
        if (dy < dst->cy0 || dy >= dst->cy1)
            continue;
        int x0 = x < dst->cx0 ? dst->cx0 : x;
        int x1 = x + src->w > dst->cx1 ? dst->cx1 : x + src->w;
        if (x1 <= x0)
            continue;
        memcpy(&dst->px[(size_t)dy * (size_t)dst->w + (size_t)x0], &src->px[(size_t)j * (size_t)src->w + (size_t)(x0 - x)],
               (size_t)(x1 - x0) * 4);
    }
}

color color_mix(color a, color b, float t) {
    if (t <= 0)
        return a;
    if (t >= 1)
        return b;
    return RGBA((int)(C_R(a) + (C_R(b) - (float)C_R(a)) * t), (int)(C_G(a) + (C_G(b) - (float)C_G(a)) * t),
                (int)(C_B(a) + (C_B(b) - (float)C_B(a)) * t), (int)(C_A(a) + (C_A(b) - (float)C_A(a)) * t));
}

color color_alpha(color c, float a) {
    int na = (int)(C_A(c) * a);
    if (na < 0)
        na = 0;
    if (na > 255)
        na = 255;
    return (c & 0x00ffffffu) | ((uint32_t)na << 24);
}

static int hexval(char ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

color color_parse(const char *s, color fallback) {
    if (!s)
        return fallback;
    if (*s == '#')
        s++;
    size_t n = strlen(s);
    if (n != 6 && n != 8)
        return fallback;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        int h = hexval(s[i]);
        if (h < 0)
            return fallback;
        v = v << 4 | (uint32_t)h;
    }
    return n == 6 ? (0xff000000u | v) : ((v >> 8) | ((v & 0xff) << 24));
}

static float lin(float c) {
    c /= 255.0f;
    return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float color_luma(color c) { return 0.2126f * lin(C_R(c)) + 0.7152f * lin(C_G(c)) + 0.0722f * lin(C_B(c)); }

float color_contrast(color a, color b) {
    float la = color_luma(a), lb = color_luma(b);
    if (la < lb) {
        float t = la;
        la = lb;
        lb = t;
    }
    return (la + 0.05f) / (lb + 0.05f);
}

void fill_rect(canvas *c, rect r, color col) {
    int x0 = r.x < c->cx0 ? c->cx0 : r.x, y0 = r.y < c->cy0 ? c->cy0 : r.y;
    int x1 = r.x + r.w > c->cx1 ? c->cx1 : r.x + r.w, y1 = r.y + r.h > c->cy1 ? c->cy1 : r.y + r.h;
    if (C_A(col) == 255) {
        for (int y = y0; y < y1; y++) {
            uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
            for (int x = x0; x < x1; x++)
                row[x] = col;
        }
        return;
    }
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            plot(c, x, y, col, 255);
}

/* Coverage of pixel (x,y) by a rounded rectangle, via signed distance. */
static inline float round_cov(rect r, float rad, float px, float py) {
    float hx = r.w * 0.5f, hy = r.h * 0.5f;
    float cx = r.x + hx, cy = r.y + hy;
    float qx = fabsf(px - cx) - (hx - rad), qy = fabsf(py - cy) - (hy - rad);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - rad;
    float cov = 0.5f - d;
    return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}



void fill_round_shaded(canvas *c, rect r, float radius, shade_fn shade, void *ctx) {
    float maxr = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > maxr)
        radius = maxr;
    if (radius < 0)
        radius = 0;
    int x0 = r.x < c->cx0 ? c->cx0 : r.x, y0 = r.y < c->cy0 ? c->cy0 : r.y;
    int x1 = r.x + r.w > c->cx1 ? c->cx1 : r.x + r.w, y1 = r.y + r.h > c->cy1 ? c->cy1 : r.y + r.h;
    int rad = (int)ceilf(radius) + 1;
    for (int y = y0; y < y1; y++) {
        bool edge_row = y < r.y + rad || y >= r.y + r.h - rad;
        uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
        for (int x = x0; x < x1; x++) {
            color col = shade(ctx, x, y);
            if (edge_row || x < r.x + rad || x >= r.x + r.w - rad) {
                float cov = round_cov(r, radius, x + 0.5f, y + 0.5f);
                if (cov <= 0)
                    continue;
                row[x] = blend(row[x], col, (uint32_t)(cov * 255));
            } else {
                row[x] = blend(row[x], col, 255);
            }
        }
    }
}

/* A rounded fill whose colour runs from `top` to `bottom` (the same when
 * flat). The same pixels as fill_round_shaded with a colour callback, but
 * one colour per row instead of a call per pixel: window and panel
 * backgrounds are hundreds of thousands of pixels. */
static void fill_round_rows(canvas *c, rect r, float radius, color top, color bottom) {
    if (!C_A(top) && !C_A(bottom))
        return;
    float maxr = (r.w < r.h ? r.w : r.h) * 0.5f;
    if (radius > maxr)
        radius = maxr;
    if (radius < 0)
        radius = 0;
    int x0 = r.x < c->cx0 ? c->cx0 : r.x, y0 = r.y < c->cy0 ? c->cy0 : r.y;
    int x1 = r.x + r.w > c->cx1 ? c->cx1 : r.x + r.w, y1 = r.y + r.h > c->cy1 ? c->cy1 : r.y + r.h;
    int rad = (int)ceilf(radius) + 1;
    int ia = r.x + rad > x0 ? r.x + rad : x0, ib = r.x + r.w - rad < x1 ? r.x + r.w - rad : x1;
    for (int y = y0; y < y1; y++) {
        color col = top == bottom ? top : color_mix(top, bottom, r.h > 1 ? (float)(y - r.y) / (float)(r.h - 1) : 0);
        uint32_t a = C_A(col), na = 255 - a;
        if (!a)
            continue;
        uint32_t pr = C_R(col) * a + 127, pg = C_G(col) * a + 127, pb = C_B(col) * a + 127;
        bool edge_row = y < r.y + rad || y >= r.y + r.h - rad;
        uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
        for (int x = x0; x < x1; x++) {
            if (edge_row || x < ia || x >= ib) {
                float cov = round_cov(r, radius, x + 0.5f, y + 0.5f);
                if (cov > 0)
                    row[x] = blend(row[x], col, (uint32_t)(cov * 255));
            } else if (a == 255) {
                row[x] = col;
            } else {
                uint32_t d = row[x];
                row[x] = 0xff000000u | ((pr + C_R(d) * na) / 255) << 16 | ((pg + C_G(d) * na) / 255) << 8 |
                         ((pb + C_B(d) * na) / 255);
            }
        }
    }
}

void fill_round(canvas *c, rect r, float radius, color col) { fill_round_rows(c, r, radius, col, col); }

void fill_round_vgradient(canvas *c, rect r, float radius, color top, color bottom) {
    fill_round_rows(c, r, radius, top, bottom);
}

void fill_vgradient(canvas *c, rect r, color top, color bottom) {
    for (int y = r.y; y < r.y + r.h; y++)
        fill_rect(c, (rect){r.x, y, r.w, 1}, color_mix(top, bottom, r.h > 1 ? (float)(y - r.y) / (float)(r.h - 1) : 0));
}

void stroke_round(canvas *c, rect r, float radius, float width, color col) {
    int x0 = r.x - 1 < c->cx0 ? c->cx0 : r.x - 1, y0 = r.y - 1 < c->cy0 ? c->cy0 : r.y - 1;
    int x1 = r.x + r.w + 1 > c->cx1 ? c->cx1 : r.x + r.w + 1, y1 = r.y + r.h + 1 > c->cy1 ? c->cy1 : r.y + r.h + 1;
    rect inner = {r.x, r.y, r.w, r.h};
    float ir = radius - width;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float outer = round_cov(r, radius, px, py);
            if (outer <= 0)
                continue;
            rect in = {inner.x + (int)width, inner.y + (int)width, inner.w - 2 * (int)width, inner.h - 2 * (int)width};
            float inn = in.w > 0 && in.h > 0 ? round_cov(in, ir > 0 ? ir : 0, px, py) : 0;
            float cov = outer - inn;
            if (cov > 0)
                plot(c, x, y, col, (uint32_t)(cov * 255));
        }
}

void fill_circle(canvas *c, float cx, float cy, float radius, color col) {
    int x0 = (int)floorf(cx - radius - 1), x1 = (int)ceilf(cx + radius + 1);
    int y0 = (int)floorf(cy - radius - 1), y1 = (int)ceilf(cy + radius + 1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = radius + 0.5f - sqrtf(dx * dx + dy * dy);
            if (cov > 0)
                plot(c, x, y, col, (uint32_t)((cov > 1 ? 1 : cov) * 255));
        }
}

/* Soft shadow: rounded rect with a smooth falloff of width `blur`. */
void shadow_round(canvas *c, rect r, float radius, float blur, color col) {
    int b = (int)ceilf(blur);
    int x0 = r.x - b, y0 = r.y - b, x1 = r.x + r.w + b, y1 = r.y + r.h + b;
    float hx = r.w * 0.5f, hy = r.h * 0.5f, cx = r.x + hx, cy = r.y + hy;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    if (x1 <= x0 || y1 <= y0)
        return;
    /* In the columns between the corners the falloff depends on the row
     * alone, so most of a big shadow is one value per row; only the side
     * bands and corners need the distance worked out per pixel. */
    float inner = hx - radius;
    int ia = (int)ceilf(cx - inner - 0.5f), ib = (int)floorf(cx + inner - 0.5f) + 1;
    if (ia < x0) ia = x0;
    if (ib > x1) ib = x1;
    if (ib < ia) ia = ib = x1;
    for (int y = y0; y < y1; y++) {
        float qy = fabsf(y + 0.5f - cy) - (hy - radius);
        for (int x = x0; x < x1; x++) {
            if (x == ia && ib > ia) {
                float d = qy > 0 ? qy - radius : -1;
                if (d < blur) {
                    float t = d <= 0 ? 1 : 1 - d / blur;
                    t = t * t * (3 - 2 * t);
                    uint32_t cov = (uint32_t)(t * 255);
                    uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
                    for (int k = ia; k < ib; k++)
                        row[k] = blend(row[k], col, cov);
                }
                x = ib - 1;
                continue;
            }
            float qx = fabsf(x + 0.5f - cx) - (hx - radius);
            float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - radius;
            if (d >= blur)
                continue;
            float t = d <= 0 ? 1 : 1 - d / blur;
            t = t * t * (3 - 2 * t);
            plot(c, x, y, col, (uint32_t)(t * 255));
        }
    }
}

/* Box blur, three passes approximate a gaussian; used for frosted panels. */
void blur_region(canvas *c, rect r, int radius) {
    if (radius < 1)
        return;
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w, y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0)
        return;
    uint32_t *tmp = malloc((size_t)(w > h ? w : h) * 4);
    if (!tmp)
        return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = y0; y < y1; y++) { /* horizontal */
            uint32_t *row = &c->px[(size_t)y * (size_t)c->w + (size_t)x0];
            uint32_t sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int i = -radius; i <= radius; i++) {
                int k = i < 0 ? 0 : i >= w ? w - 1 : i;
                sr += C_R(row[k]), sg += C_G(row[k]), sb += C_B(row[k]), n++;
            }
            for (int x = 0; x < w; x++) {
                tmp[x] = RGBA(sr / (uint32_t)n, sg / (uint32_t)n, sb / (uint32_t)n, 255);
                int out = x - radius < 0 ? 0 : x - radius, in = x + radius + 1 >= w ? w - 1 : x + radius + 1;
                sr += C_R(row[in]) - C_R(row[out]), sg += C_G(row[in]) - C_G(row[out]), sb += C_B(row[in]) - C_B(row[out]);
            }
            memcpy(row, tmp, (size_t)w * 4);
        }
        for (int x = x0; x < x1; x++) { /* vertical */
            uint32_t sr = 0, sg = 0, sb = 0;
            int n = 0;
#define PX(yy) c->px[(size_t)(y0 + (yy)) * (size_t)c->w + (size_t)x]
            for (int i = -radius; i <= radius; i++) {
                int k = i < 0 ? 0 : i >= h ? h - 1 : i;
                sr += C_R(PX(k)), sg += C_G(PX(k)), sb += C_B(PX(k)), n++;
            }
            for (int y = 0; y < h; y++) {
                tmp[y] = RGBA(sr / (uint32_t)n, sg / (uint32_t)n, sb / (uint32_t)n, 255);
                int out = y - radius < 0 ? 0 : y - radius, in = y + radius + 1 >= h ? h - 1 : y + radius + 1;
                sr += C_R(PX(in)) - C_R(PX(out)), sg += C_G(PX(in)) - C_G(PX(out)), sb += C_B(PX(in)) - C_B(PX(out));
            }
            for (int y = 0; y < h; y++)
                PX(y) = tmp[y];
#undef PX
        }
    }
    free(tmp);
}

/* Frosted glass with rounded corners: blur the rectangle, then put back the
 * original pixels outside the rounded shape (blending along the edge). */
void blur_round(canvas *c, rect r, float radius, int strength) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w, y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    if (x1 <= x0 || y1 <= y0)
        return;
    int rad = (int)ceilf(radius) + 1;
    /* Only the corner squares need restoring; save them first. */
    int cw = rad < (x1 - x0) / 2 ? rad : (x1 - x0) / 2, ch = rad < (y1 - y0) / 2 ? rad : (y1 - y0) / 2;
    uint32_t *saved = malloc((size_t)(4 * cw * ch) * 4);
    if (!saved) {
        blur_region(c, r, strength);
        return;
    }
    int corners[4][2] = {{x0, y0}, {x1 - cw, y0}, {x0, y1 - ch}, {x1 - cw, y1 - ch}};
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < ch; j++)
            memcpy(&saved[(size_t)(k * cw * ch + j * cw)], &c->px[(size_t)(corners[k][1] + j) * (size_t)c->w + (size_t)corners[k][0]],
                   (size_t)cw * 4);
    blur_region(c, r, strength);
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < ch; j++)
            for (int i = 0; i < cw; i++) {
                int x = corners[k][0] + i, y = corners[k][1] + j;
                float cov = round_cov(r, radius, x + 0.5f, y + 0.5f);
                uint32_t orig = saved[(size_t)(k * cw * ch + j * cw + i)];
                uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
                if (cov <= 0)
                    *p = orig;
                else if (cov < 1)
                    *p = blend(orig, *p | 0xff000000u, (uint32_t)(cov * 255));
            }
    free(saved);
}

void draw_line(canvas *c, float x0, float y0, float x1, float y1, float width, color col) {
    float minx = fminf(x0, x1) - width, maxx = fmaxf(x0, x1) + width;
    float miny = fminf(y0, y1) - width, maxy = fmaxf(y0, y1) + width;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = (int)miny; y <= (int)maxy; y++)
        for (int x = (int)minx; x <= (int)maxx; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = px - t * dx, ey = py - t * dy;
            float cov = width * 0.5f + 0.5f - sqrtf(ex * ex + ey * ey);
            if (cov > 0)
                plot(c, x, y, col, (uint32_t)((cov > 1 ? 1 : cov) * 255));
        }
}

void glow_circle(canvas *c, float cx, float cy, float radius, color col) {
    int x0 = (int)(cx - radius), x1 = (int)(cx + radius) + 1, y0 = (int)(cy - radius), y1 = (int)(cy + radius) + 1;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    float inv = 1.0f / (radius * radius);
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - cy;
        uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, d2 = (dx * dx + dy * dy) * inv;
            if (d2 >= 1)
                continue;
            float t = 1 - d2;
            t = t * t; /* smooth falloff */
            row[x] = blend(row[x], col, (uint32_t)(t * 255));
        }
    }
}

void glow_round(canvas *c, rect r, float radius, float spread, color col) { shadow_round(c, r, radius, spread, col); }

void stroke_round_vgradient(canvas *c, rect r, float radius, float width, color top, color bottom) {
    int x0 = r.x - 1 < c->cx0 ? c->cx0 : r.x - 1, y0 = r.y - 1 < c->cy0 ? c->cy0 : r.y - 1;
    int x1 = r.x + r.w + 1 > c->cx1 ? c->cx1 : r.x + r.w + 1, y1 = r.y + r.h + 1 > c->cy1 ? c->cy1 : r.y + r.h + 1;
    rect in = {r.x + (int)width, r.y + (int)width, r.w - 2 * (int)width, r.h - 2 * (int)width};
    float ir = radius - width > 0 ? radius - width : 0;
    int rad = (int)ceilf(radius) + 2;
    for (int y = y0; y < y1; y++) {
        color col = color_mix(top, bottom, r.h > 1 ? (float)(y - r.y) / (float)(r.h - 1) : 0);
        bool edge_row = y < r.y + rad || y >= r.y + r.h - rad;
        for (int x = x0; x < x1; x++) {
            if (!edge_row && x > r.x + (int)width + 1 && x < r.x + r.w - (int)width - 2)
                continue; /* interior of a straight run: skip quickly */
            float px = x + 0.5f, py = y + 0.5f;
            float outer = round_cov(r, radius, px, py);
            if (outer <= 0)
                continue;
            float inn = in.w > 0 && in.h > 0 ? round_cov(in, ir, px, py) : 0;
            float cov = outer - inn;
            if (cov > 0)
                plot(c, x, y, col, (uint32_t)(cov * 255));
        }
    }
}

void vibrancy(canvas *c, rect r, float radius, float saturate, float lift) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w, y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    int rad = (int)ceilf(radius) + 1;
    /* Fixed point (8 fractional bits): the same sums as in floats, a few
     * times faster over a window-sized panel. */
    int s = (int)(saturate * 256 + 0.5f), lf = (int)(lift * 256 + (lift < 0 ? -0.5f : 0.5f));
    for (int y = y0; y < y1; y++) {
        uint32_t *row = &c->px[(size_t)y * (size_t)c->w];
        bool edge_row = y < r.y + rad || y >= r.y + r.h - rad;
        for (int x = x0; x < x1; x++) {
            if ((edge_row || x < r.x + rad || x >= r.x + r.w - rad) && round_cov(r, radius, x + 0.5f, y + 0.5f) < 0.5f)
                continue;
            uint32_t p = row[x];
            int R = (int)C_R(p), G = (int)C_G(p), B = (int)C_B(p);
            int l = (77 * R + 150 * G + 29 * B) >> 8;
            R = l + (((R - l) * s + lf) >> 8), G = l + (((G - l) * s + lf) >> 8), B = l + (((B - l) * s + lf) >> 8);
            R = R < 0 ? 0 : R > 255 ? 255 : R, G = G < 0 ? 0 : G > 255 ? 255 : G, B = B < 0 ? 0 : B > 255 ? 255 : B;
            row[x] = RGBA(R, G, B, 255);
        }
    }
}

void draw_arc(canvas *c, float cx, float cy, float radius, float a0, float a1, float width, color col) {
    if (a1 < a0) {
        float t = a0;
        a0 = a1;
        a1 = t;
    }
    float half = width * 0.5f;
    int x0 = (int)(cx - radius - width), x1 = (int)(cx + radius + width) + 1;
    int y0 = (int)(cy - radius - width), y1 = (int)(cy + radius + width) + 1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = half + 0.5f - fabsf(d - radius);
            if (cov <= 0)
                continue;
            float a = atan2f(dy, dx);
            /* normalise into [a0, a0 + 2pi) */
            while (a < a0)
                a += 6.2831853f;
            while (a >= a0 + 6.2831853f)
                a -= 6.2831853f;
            if (a > a1)
                continue;
            plot(c, x, y, col, (uint32_t)((cov > 1 ? 1 : cov) * 255));
        }
    /* round caps */
    fill_circle(c, cx + cosf(a0) * radius, cy + sinf(a0) * radius, half, col);
    fill_circle(c, cx + cosf(a1) * radius, cy + sinf(a1) * radius, half, col);
}

void canvas_scale(canvas *dst, const canvas *src) {
    float sx = (float)(src->w - 1) / (float)(dst->w - 1), sy = (float)(src->h - 1) / (float)(dst->h - 1);
    for (int y = 0; y < dst->h; y++) {
        float fy = y * sy;
        int y0 = (int)fy, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
        uint32_t ty = (uint32_t)((fy - y0) * 256);
        const uint32_t *r0 = &src->px[(size_t)y0 * (size_t)src->w], *r1 = &src->px[(size_t)y1 * (size_t)src->w];
        uint32_t *out = &dst->px[(size_t)y * (size_t)dst->w];
        for (int x = 0; x < dst->w; x++) {
            float fx = x * sx;
            int x0 = (int)fx, x1 = x0 + 1 < src->w ? x0 + 1 : x0;
            uint32_t tx = (uint32_t)((fx - x0) * 256);
            uint32_t a = r0[x0], b = r0[x1], cc = r1[x0], d = r1[x1];
#define LERP(p, q, t) (((p) * (256 - (t)) + (q) * (t)) >> 8)
#define CH(v, s) (((v) >> (s)) & 0xff)
            uint32_t rr = LERP(LERP(CH(a, 16), CH(b, 16), tx), LERP(CH(cc, 16), CH(d, 16), tx), ty);
            uint32_t gg = LERP(LERP(CH(a, 8), CH(b, 8), tx), LERP(CH(cc, 8), CH(d, 8), tx), ty);
            uint32_t bb = LERP(LERP(CH(a, 0), CH(b, 0), tx), LERP(CH(cc, 0), CH(d, 0), tx), ty);
#undef LERP
#undef CH
            out[x] = 0xff000000u | rr << 16 | gg << 8 | bb;
        }
    }
}

/* Half-resolution blur: copy the region down 2x, box-blur it, scale back up
 * with bilinear filtering, and keep the rounded corners crisp. */
/* Scratch memory kept between calls: blurs run many times a frame, and
 * allocating (and zeroing) a window-sized buffer each time cost as much as
 * the blur itself. */
static void *scratch(int slot, size_t bytes) {
    static void *buf[4];
    static size_t cap[4];
    if (bytes > cap[slot]) {
        free(buf[slot]);
        buf[slot] = malloc(bytes);
        cap[slot] = buf[slot] ? bytes : 0;
    }
    return buf[slot];
}

/* Three box blurs of a w x h picture (close to a gaussian), the same sums
 * as blur_region but read in memory order: rows with a running sum, then
 * columns a whole row at a time with a running sum per column. */
static void box_blur3(uint32_t *px, uint32_t *tmp, uint32_t *acc, int w, int h, int radius) {
    int n = 2 * radius + 1;
    uint32_t inv = (65536u + (uint32_t)n / 2) / (uint32_t)n;
#define AVG(s) ((((s) * inv) + 32768u) >> 16)
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) { /* horizontal: px -> tmp */
            const uint32_t *row = &px[(size_t)y * (size_t)w];
            uint32_t *out = &tmp[(size_t)y * (size_t)w];
            uint32_t sr = 0, sg = 0, sb = 0;
            for (int i = -radius; i <= radius; i++) {
                int k = i < 0 ? 0 : i >= w ? w - 1 : i;
                sr += C_R(row[k]), sg += C_G(row[k]), sb += C_B(row[k]);
            }
            for (int x = 0; x < w; x++) {
                out[x] = 0xff000000u | AVG(sr) << 16 | AVG(sg) << 8 | AVG(sb);
                int o = x - radius < 0 ? 0 : x - radius, in = x + radius + 1 >= w ? w - 1 : x + radius + 1;
                sr += C_R(row[in]) - C_R(row[o]), sg += C_G(row[in]) - C_G(row[o]), sb += C_B(row[in]) - C_B(row[o]);
            }
        }
        uint32_t *ar = acc, *ag = acc + w, *ab = acc + 2 * w; /* vertical: tmp -> px */
        memset(acc, 0, (size_t)w * 3 * 4);
        for (int i = -radius; i <= radius; i++) {
            const uint32_t *row = &tmp[(size_t)(i < 0 ? 0 : i >= h ? h - 1 : i) * (size_t)w];
            for (int x = 0; x < w; x++)
                ar[x] += C_R(row[x]), ag[x] += C_G(row[x]), ab[x] += C_B(row[x]);
        }
        for (int y = 0; y < h; y++) {
            uint32_t *out = &px[(size_t)y * (size_t)w];
            for (int x = 0; x < w; x++)
                out[x] = 0xff000000u | AVG(ar[x]) << 16 | AVG(ag[x]) << 8 | AVG(ab[x]);
            int o = y - radius < 0 ? 0 : y - radius, in = y + radius + 1 >= h ? h - 1 : y + radius + 1;
            const uint32_t *ri = &tmp[(size_t)in * (size_t)w], *ro = &tmp[(size_t)o * (size_t)w];
            for (int x = 0; x < w; x++)
                ar[x] += C_R(ri[x]) - C_R(ro[x]), ag[x] += C_G(ri[x]) - C_G(ro[x]), ab[x] += C_B(ri[x]) - C_B(ro[x]);
        }
    }
#undef AVG
}

void blur_fast(canvas *c, rect r, float radius, int strength) {
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > c->w ? c->w : r.x + r.w, y1 = r.y + r.h > c->h ? c->h : r.y + r.h;
    int w = (x1 - x0) / 2, h = (y1 - y0) / 2;
    if (w < 4 || h < 4) {
        blur_round(c, r, radius, strength);
        return;
    }
    int bw = x1 - x0, bh = y1 - y0;
    uint32_t *small = scratch(0, (size_t)w * (size_t)h * 4), *tmp = scratch(1, (size_t)w * (size_t)h * 4);
    uint32_t *acc = scratch(2, (size_t)w * 3 * 4);
    int *xs = scratch(3, (size_t)bw * 2 * sizeof(int));
    if (!small || !tmp || !acc || !xs)
        return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t *p = &c->px[(size_t)(y0 + 2 * y) * (size_t)c->w + (size_t)(x0 + 2 * x)];
            uint32_t a = p[0], b = p[1], d = p[c->w], e = p[c->w + 1];
            small[(size_t)y * (size_t)w + (size_t)x] =
                0xff000000u | (((C_R(a) + C_R(b) + C_R(d) + C_R(e)) / 4) << 16) |
                (((C_G(a) + C_G(b) + C_G(d) + C_G(e)) / 4) << 8) | ((C_B(a) + C_B(b) + C_B(d) + C_B(e)) / 4);
        }
    box_blur3(small, tmp, acc, w, h, strength / 2 > 1 ? strength / 2 : 1);
    /* Back up to full size with bilinear filtering (the mapping canvas_scale
     * uses), straight into the canvas, keeping the rounded corners crisp. */
    float sx = (float)(w - 1) / (float)(bw - 1), sy = (float)(h - 1) / (float)(bh - 1);
    for (int x = 0; x < bw; x++) {
        float fx = x * sx;
        xs[2 * x] = (int)fx;
        xs[2 * x + 1] = (int)((fx - (int)fx) * 256);
    }
    int rad = (int)ceilf(radius) + 1;
    for (int y = 0; y < bh; y++) {
        int gy = y0 + y;
        float fy = y * sy;
        int sy0 = (int)fy, sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
        uint32_t ty = (uint32_t)((fy - sy0) * 256);
        const uint32_t *r0 = &small[(size_t)sy0 * (size_t)w], *r1 = &small[(size_t)sy1 * (size_t)w];
        uint32_t *dst = &c->px[(size_t)gy * (size_t)c->w + (size_t)x0];
        bool edge_row = gy < r.y + rad || gy >= r.y + r.h - rad;
        int a = edge_row ? bw : rad, b = edge_row ? bw : bw - rad;
        for (int x = 0; x < bw; x++) {
            int i0 = xs[2 * x], i1 = i0 + 1 < w ? i0 + 1 : i0;
            uint32_t tx = (uint32_t)xs[2 * x + 1];
            uint32_t p = r0[i0], q = r0[i1], u = r1[i0], v = r1[i1];
#define LERP(m, n, t) (((m) * (256 - (t)) + (n) * (t)) >> 8)
#define CH(val, s) (((val) >> (s)) & 0xff)
            uint32_t rr = LERP(LERP(CH(p, 16), CH(q, 16), tx), LERP(CH(u, 16), CH(v, 16), tx), ty);
            uint32_t gg = LERP(LERP(CH(p, 8), CH(q, 8), tx), LERP(CH(u, 8), CH(v, 8), tx), ty);
            uint32_t bb = LERP(LERP(CH(p, 0), CH(q, 0), tx), LERP(CH(u, 0), CH(v, 0), tx), ty);
#undef LERP
#undef CH
            uint32_t src = 0xff000000u | rr << 16 | gg << 8 | bb;
            if (x >= a && x < b) {
                dst[x] = src;
                continue;
            }
            float cov = round_cov(r, radius, x0 + x + 0.5f, gy + 0.5f);
            dst[x] = cov >= 1 ? src : cov <= 0 ? dst[x] : blend(dst[x], src, (uint32_t)(cov * 255));
        }
    }
}

bool pt_in(rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

rect rect_inset(rect r, int d) { return (rect){r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }
