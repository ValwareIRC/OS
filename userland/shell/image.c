/* stb_image is compiled here, privately (static), so it can't collide with
 * any other copy of it in the shell. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_PIC
#include "stb_image.h"
#pragma GCC diagnostic pop

#include "image.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Area-averaging resize: every source pixel counts, so shrinking a photo
 * by ten keeps it smooth instead of sparkling. Enlarging repeats pixels. */
static image *resize_rgba(const uint8_t *src, int sw, int sh, int dw, int dh, int src_w, int src_h) {
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    image *im = calloc(1, sizeof *im);
    if (!im)
        return NULL;
    im->px = malloc((size_t)dw * (size_t)dh * 4);
    if (!im->px) {
        free(im);
        return NULL;
    }
    im->w = dw, im->h = dh, im->src_w = src_w, im->src_h = src_h;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh), y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0)
            y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw), x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0)
                x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int j = y0; j < y1 && j < sh; j++) {
                const uint8_t *p = src + ((size_t)j * (size_t)sw + (size_t)x0) * 4;
                for (int i = x0; i < x1 && i < sw; i++, p += 4) {
                    /* weight colour by alpha so transparent edges don't go grey */
                    r += p[0] * p[3], g += p[1] * p[3], b += p[2] * p[3], a += p[3], n++;
                }
            }
            uint32_t out = 0;
            if (a)
                out = (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a);
            im->px[(size_t)y * (size_t)dw + (size_t)x] = out;
        }
    }
    return im;
}

image *image_load(const char *path, int max_side) {
    int w, h, ch;
    uint8_t *data = stbi_load(path, &w, &h, &ch, 4);
    if (!data)
        return NULL;
    int dw = w, dh = h;
    if (max_side > 0 && (w > max_side || h > max_side)) {
        if (w >= h)
            dw = max_side, dh = (int)((long long)h * max_side / w);
        else
            dh = max_side, dw = (int)((long long)w * max_side / h);
    }
    image *im = resize_rgba(data, w, h, dw, dh, w, h);
    stbi_image_free(data);
    return im;
}

image *image_resized(const image *im, int w, int h) {
    /* back to bytes, so one resize routine serves both */
    uint8_t *rgba = malloc((size_t)im->w * (size_t)im->h * 4);
    if (!rgba)
        return NULL;
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++) {
        uint32_t p = im->px[i];
        rgba[i * 4] = (uint8_t)(p >> 16), rgba[i * 4 + 1] = (uint8_t)(p >> 8), rgba[i * 4 + 2] = (uint8_t)p;
        rgba[i * 4 + 3] = (uint8_t)(p >> 24);
    }
    image *out = resize_rgba(rgba, im->w, im->h, w, h, im->src_w, im->src_h);
    free(rgba);
    return out;
}

void image_free(image *im) {
    if (!im)
        return;
    free(im->px);
    free(im);
}

void image_draw(canvas *c, const image *im, int x, int y, float opacity) {
    if (!im)
        return;
    uint32_t op = (uint32_t)(opacity * 256);
    for (int j = 0; j < im->h; j++) {
        int dy = y + j;
        if (dy < c->cy0 || dy >= c->cy1)
            continue;
        const uint32_t *s = &im->px[(size_t)j * (size_t)im->w];
        uint32_t *d = &c->px[(size_t)dy * (size_t)c->w];
        for (int i = 0; i < im->w; i++) {
            int dx = x + i;
            if (dx < c->cx0 || dx >= c->cx1)
                continue;
            uint32_t p = s[i], a = ((p >> 24) * op) >> 8;
            if (a >= 255) {
                d[dx] = p | 0xff000000u;
                continue;
            }
            if (!a)
                continue;
            uint32_t q = d[dx], na = 255 - a;
            uint32_t r = (((p >> 16) & 0xff) * a + ((q >> 16) & 0xff) * na) / 255;
            uint32_t g = (((p >> 8) & 0xff) * a + ((q >> 8) & 0xff) * na) / 255;
            uint32_t b = ((p & 0xff) * a + (q & 0xff) * na) / 255;
            d[dx] = 0xff000000u | r << 16 | g << 8 | b;
        }
    }
}

bool image_name_ok(const char *name) {
    static const char *const exts[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".ppm", ".pgm", ".pnm", NULL};
    size_t n = strlen(name);
    for (int i = 0; exts[i]; i++) {
        size_t e = strlen(exts[i]);
        if (n > e && !strcasecmp(name + n - e, exts[i]))
            return true;
    }
    return false;
}
