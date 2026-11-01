/* Pictures in a design: <image src="..."/> (PNG, JPEG, GIF, BMP or SVG on
 * this computer) and <svg>...</svg> written right in the markup, for logos
 * and marks. SVG is drawn by nanosvg (zlib licence, fm/nanosvg*.h); the
 * rest by stb_image (image.c). Drawn at the size they are shown, kept until
 * that size or the picture changes. */
#define _GNU_SOURCE
#include "fmi.h"
#include "../image.h"
#include "../../lib/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "nanosvg.h"
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

#define MAX_SIDE 2048
#define CACHED 24

static struct pic {
    char key[320]; /* the source (a path, or "inline:" and a hash) and the size */
    int w, h;
    uint32_t *px; /* ARGB, straight alpha */
    uint64_t used;
} cache[CACHED];
static uint64_t clock_n;

static unsigned long hash_of(const char *s) {
    unsigned long h = 5381;
    for (; *s; s++)
        h = h * 33 + (unsigned char)*s;
    return h;
}

/* A path a design may show: on this computer, a picture, and not somewhere private. */
static bool path_ok(const char *src) {
    if (!src || !src[0] || strstr(src, "://") || strstr(src, ".."))
        return false;
    if (!strncmp(src, "/etc/", 5) && strncmp(src, "/etc/os/", 8))
        return false;
    if (!strncmp(src, "/proc/", 6) || !strncmp(src, "/sys/", 5) || !strncmp(src, "/dev/", 5))
        return false;
    size_t n = strlen(src);
    return (n > 4 && !strcasecmp(src + n - 4, ".svg")) || image_name_ok(src);
}

static const char *resolve(const char *src, char *buf, size_t n) {
    if (src[0] == '/')
        return src;
    if (src[0] == '~' && src[1] == '/') {
        snprintf(buf, n, "%s/%s", getenv("HOME") ? getenv("HOME") : "/root", src + 2);
        return buf;
    }
    snprintf(buf, n, "/usr/share/os/pictures/%s", src); /* pictures that come with the system */
    return buf;
}

static char *svg_text_of(const fm_node *n) {
    /* the inner markup, kept raw by the reader, wrapped back in its own <svg ...> */
    sbuf b;
    sb_init(&b);
    sb_add(&b, "<svg");
    for (int i = 0; i < n->nattrs; i++)
        sb_printf(&b, " %s=\"%s\"", n->attrs[i].name, n->attrs[i].value);
    if (!fm_attr_get(n, "xmlns"))
        sb_add(&b, " xmlns=\"http://www.w3.org/2000/svg\"");
    sb_add(&b, ">");
    for (const fm_node *c = n->first; c; c = c->next)
        if (c->kind == FM_TEXT && c->text)
            sb_add(&b, c->text);
    sb_add(&b, "</svg>");
    return b.p;
}

static NSVGimage *svg_parse(const fm_node *n, const char *path) {
    if (path) {
        struct stat st;
        if (stat(path, &st) != 0 || st.st_size > (4 << 20))
            return NULL;
        return nsvgParseFromFile(path, "px", 96);
    }
    char *t = svg_text_of(n);
    NSVGimage *im = t ? nsvgParse(t, "px", 96) : NULL; /* (nsvgParse writes into its text) */
    free(t);
    return im;
}

/* ---- <pixels>: pixel art written as rows of letters ----
 *   <pixels palette="k #000, w #fff, r #f03c24" scale="2">
 *   ..kkkk..
 *   .krrrrk.
 *   </pixels>
 * Each letter is a pixel of its colour; "." and spaces are clear. Drawn by whole pixels, never blurred. */
typedef struct {
    int cols, rows;
    const char *row[256];
    int len[256];
    color pal[128];
    bool has[128];
} grid;

static bool pixels_read(const fm_node *n, grid *g) {
    memset(g, 0, sizeof *g);
    const char *pal = fm_attr_get(n, "palette");
    for (const char *p = pal ? pal : ""; *p;) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '\n')
            p++;
        if (!*p)
            break;
        unsigned char key = (unsigned char)*p++;
        while (*p == ' ' || *p == ':' || *p == '=')
            p++;
        char val[64];
        int k = 0;
        while (*p && *p != ',' && *p != ';' && !(*p == ' ' && k && val[0] == '#') && k < 63)
            val[k++] = *p++;
        val[k] = 0;
        while (k && val[k - 1] == ' ')
            val[--k] = 0;
        color c;
        if (key < 128 && fm_parse_color(val, &c))
            g->pal[key] = c, g->has[key] = true;
    }
    const char *t = NULL;
    for (const fm_node *c = n->first; c; c = c->next)
        if (c->kind == FM_TEXT && c->text)
            t = c->text;
    for (const char *p = t ? t : ""; *p && g->rows < 256;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        const char *s = p;
        size_t m = l;
        while (m && (*s == ' ' || *s == '\t'))
            s++, m--; /* the indentation of the markup isn't part of the picture */
        while (m && (s[m - 1] == ' ' || s[m - 1] == '\r' || s[m - 1] == '\t'))
            m--;
        if (m) {
            g->row[g->rows] = s;
            g->len[g->rows] = (int)m;
            if ((int)m > g->cols)
                g->cols = (int)m;
            g->rows++;
        }
        p = e ? e + 1 : p + l;
    }
    return g->rows > 0 && g->cols > 0;
}

static void pixels_draw(canvas *c, const fm_node *n, rect r, float op) {
    static grid g;
    if (!pixels_read(n, &g))
        return;
    int sx = r.w / g.cols, sy = r.h / g.rows, s = sx < sy ? sx : sy;
    if (s < 1)
        s = 1;
    int ox = r.x + (r.w - g.cols * s) / 2, oy = r.y + (r.h - g.rows * s) / 2;
    for (int y = 0; y < g.rows; y++)
        for (int x = 0; x < g.len[y]; x++) {
            unsigned char k = (unsigned char)g.row[y][x];
            if (k >= 128 || !g.has[k])
                continue;
            fill_rect(c, (rect){ox + x * s, oy + y * s, s, s}, color_alpha(g.pal[k], op));
        }
}

/* The picture's own size, for laying it out when the design gives none. */
bool fmi_media_size(const fm_node *n, float *w, float *h) {
    char buf[600];
    if (tag_is(n, "pixels")) {
        static grid g;
        if (!pixels_read(n, &g))
            return false;
        const char *sc = fm_attr_get(n, "scale");
        int s = sc ? atoi(sc) : 2;
        s = s < 1 ? 1 : s > 32 ? 32 : s;
        *w = (float)(g.cols * s);
        *h = (float)(g.rows * s);
        return true;
    }
    bool svg = tag_is(n, "svg");
    const char *src = svg ? NULL : fm_attr_get(n, "src");
    if (!svg && !path_ok(src))
        return false;
    const char *path = svg ? NULL : resolve(src, buf, sizeof buf);
    size_t pl = path ? strlen(path) : 0;
    if (svg || (pl > 4 && !strcasecmp(path + pl - 4, ".svg"))) {
        NSVGimage *im = svg_parse(n, path);
        if (!im)
            return false;
        *w = im->width > 0 ? im->width : 24;
        *h = im->height > 0 ? im->height : 24;
        nsvgDelete(im);
        return true;
    }
    image *im = image_load(path, 1); /* (stb reads the size from the header; the 1px copy is cheap) */
    if (!im)
        return false;
    *w = (float)im->src_w;
    *h = (float)im->src_h;
    image_free(im);
    return true;
}

static struct pic *render(const fm_node *n, int w, int h) {
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE)
        return NULL;
    char key[320], buf[600];
    bool svg = tag_is(n, "svg");
    const char *src = svg ? NULL : fm_attr_get(n, "src");
    if (!svg && !path_ok(src))
        return NULL;
    const char *fit = fm_attr_get(n, "fit");
    if (svg) {
        char *t = svg_text_of(n);
        snprintf(key, sizeof key, "inline:%lx %dx%d %s", hash_of(t ? t : ""), w, h, fit ? fit : "");
        free(t);
    } else {
        snprintf(key, sizeof key, "%.260s %dx%d %s%s", src, w, h, fit ? fit : "", n->st.pixelated ? " px" : "");
    }
    struct pic *slot = &cache[0];
    for (int i = 0; i < CACHED; i++) {
        if (cache[i].px && !strcmp(cache[i].key, key)) {
            cache[i].used = ++clock_n;
            return &cache[i];
        }
        if (cache[i].used < slot->used)
            slot = &cache[i];
    }
    const char *path = svg ? NULL : resolve(src, buf, sizeof buf);
    size_t pl = path ? strlen(path) : 0;
    uint32_t *px = calloc((size_t)w * (size_t)h, 4);
    if (!px)
        return NULL;
    bool cover = fit && !strcasecmp(fit, "cover"), fill = fit && !strcasecmp(fit, "fill");
    if (svg || (pl > 4 && !strcasecmp(path + pl - 4, ".svg"))) {
        NSVGimage *im = svg_parse(n, path);
        NSVGrasterizer *rz = im ? nsvgCreateRasterizer() : NULL;
        if (!rz) {
            if (im)
                nsvgDelete(im);
            free(px);
            return NULL;
        }
        float iw = im->width > 0 ? im->width : (float)w, ih = im->height > 0 ? im->height : (float)h;
        float sx = w / iw, sy = h / ih, s = cover ? fmaxf(sx, sy) : fminf(sx, sy);
        unsigned char *rgba = malloc((size_t)w * (size_t)h * 4);
        if (rgba) {
            float tx = fill ? 0 : (w - iw * s) / 2, ty = fill ? 0 : (h - ih * s) / 2;
            if (fill) /* nanosvg scales evenly; "fill" is drawn as contain */
                tx = (w - iw * s) / 2, ty = (h - ih * s) / 2;
            nsvgRasterize(rz, im, tx, ty, s, rgba, w, h, w * 4);
            for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
                unsigned a = rgba[i * 4 + 3];
                /* nanosvg gives premultiplied colour: straightened */
                unsigned r = a ? rgba[i * 4] * 255u / a : 0, g = a ? rgba[i * 4 + 1] * 255u / a : 0,
                         b = a ? rgba[i * 4 + 2] * 255u / a : 0;
                px[i] = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
            free(rgba);
        }
        nsvgDeleteRasterizer(rz);
        nsvgDelete(im);
    } else {
        image *im = image_load(path, MAX_SIDE);
        if (!im) {
            free(px);
            return NULL;
        }
        float sx = (float)w / im->w, sy = (float)h / im->h;
        float s = fill ? 0 : cover ? fmaxf(sx, sy) : fminf(sx, sy);
        int dw = fill ? w : (int)lroundf(im->w * s), dh = fill ? h : (int)lroundf(im->h * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        image *sized = n->st.pixelated ? NULL : image_resized(im, dw, dh);
        if (n->st.pixelated) { /* nearest pixel: hard edges kept */
            sized = calloc(1, sizeof *sized);
            if (sized) {
                sized->w = dw, sized->h = dh, sized->src_w = im->src_w, sized->src_h = im->src_h;
                sized->px = malloc((size_t)dw * (size_t)dh * 4);
                for (int y = 0; sized->px && y < dh; y++)
                    for (int x = 0; x < dw; x++)
                        sized->px[(size_t)y * (size_t)dw + (size_t)x] =
                            im->px[(size_t)(y * im->h / dh) * (size_t)im->w + (size_t)(x * im->w / dw)];
            }
        }
        if (sized) {
            int ox = (w - dw) / 2, oy = (h - dh) / 2;
            for (int y = 0; y < dh; y++)
                for (int x = 0; x < dw; x++) {
                    int X = x + ox, Y = y + oy;
                    if (X >= 0 && Y >= 0 && X < w && Y < h)
                        px[(size_t)Y * (size_t)w + (size_t)X] = sized->px[(size_t)y * (size_t)dw + (size_t)x];
                }
            image_free(sized);
        }
        image_free(im);
    }
    free(slot->px);
    snprintf(slot->key, sizeof slot->key, "%s", key);
    slot->w = w;
    slot->h = h;
    slot->px = px;
    slot->used = ++clock_n;
    return slot;
}

/* Draws it in r; false if it couldn't be (missing, not a picture, not allowed). */
bool fmi_media_draw(canvas *c, const fm_node *n, rect r, float op) {
    if (tag_is(n, "pixels")) {
        pixels_draw(c, n, r, op);
        return true;
    }
    struct pic *p = render(n, r.w, r.h);
    if (!p)
        return false;
    image im = {p->w, p->h, p->px, p->w, p->h};
    image_draw(c, &im, r.x, r.y, op);
    return true;
}
