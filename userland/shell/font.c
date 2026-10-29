#define _GNU_SOURCE /* strcasestr */
#pragma GCC diagnostic ignored "-Wunused-function"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "font.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Faces are files: "Inter.ttf" is the family's regular, "Inter-Bold.ttf",
 * "Inter-Italic.ttf" and so on are its other faces. The family is the name
 * up to the first '-'; weight and slant come from the font's own OS/2
 * table, so a file can be called anything. */
#define MAX_FACES 96
#define MAX_FONTS 128
#define CACHE_GLYPHS 512
/* Rasterised glyphs, all sizes together. Past this the least recently used
 * fonts drop their glyphs (they are redrawn when needed again). */
#define GLYPH_BUDGET (12u << 20)
/* Synthetic oblique: the 12 degree shear type designers use for slanted
 * romans when a family has no italic. */
#define SLANT 0.2126f

struct face {
    char family[32];
    int weight;  /* 100..900 */
    bool italic;
    unsigned char *data;
    char *path; /* other faces load when first used */
    stbtt_fontinfo info;
};

struct glyph {
    int cp;
    int w, h, xoff, yoff, advance;
    unsigned char *bitmap;
};

/* One face at one size, weight and slant: the face may be the family's real
 * bold or italic, or a nearer one that is emboldened or slanted here. */
struct font {
    struct face *face;
    float px, scale;
    int weight;     /* asked for */
    bool italic;    /* asked for */
    float bold;     /* synthetic emboldening: extra stem width in px */
    float slant;    /* synthetic oblique shear (0 or SLANT) */
    float tracking; /* letter-spacing in px, between characters */
    int ascent, descent, line_gap;
    unsigned used;  /* for least-recently-used reuse */
    size_t bytes;   /* glyph bitmaps held */
    struct glyph cache[CACHE_GLYPHS];
};

static struct face faces[MAX_FACES];
static int nfaces;
static font fonts[MAX_FONTS];
static int nfonts;
static const char *family_names[MAX_FACES];
static int nfamilies;
static unsigned tick;
static size_t glyph_bytes;

static int be16(const unsigned char *p) { return p[0] << 8 | p[1]; }

/* Weight and italic from the OS/2 table, the file name as a fallback
 * ("Inter-SemiBoldItalic"). */
static void face_style(struct face *fc, const char *style) {
    stbtt_uint32 os2 = stbtt__find_table(fc->data, (stbtt_uint32)fc->info.fontstart, "OS/2");
    fc->weight = 400;
    fc->italic = false;
    if (os2) {
        int w = be16(fc->data + os2 + 4);
        if (w >= 100 && w <= 1000)
            fc->weight = w;
        fc->italic = (be16(fc->data + os2 + 62) & 0x201) != 0; /* ITALIC or OBLIQUE */
    } else if (style) {
        static const struct { const char *n; int w; } names[] = {
            {"thin", 100}, {"extralight", 200}, {"light", 300}, {"medium", 500}, {"semibold", 600},
            {"extrabold", 800}, {"bold", 700}, {"black", 900}};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (strcasestr(style, names[i].n)) {
                fc->weight = names[i].w;
                break;
            }
        fc->italic = strcasestr(style, "italic") != NULL;
    }
}

void fonts_init(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nfaces < MAX_FACES) {
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".ttf"))
            continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *data = malloc((size_t)size);
        struct face *fc = &faces[nfaces];
        if (data && fread(data, 1, (size_t)size, f) == (size_t)size &&
            stbtt_InitFont(&fc->info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
            const char *dash = memchr(e->d_name, '-', n - 4);
            int flen = dash ? (int)(dash - e->d_name) : (int)(n - 4);
            snprintf(fc->family, sizeof fc->family, "%.*s", flen, e->d_name);
            fc->data = data;
            char style[64];
            snprintf(style, sizeof style, "%.*s", dash ? (int)(n - 4 - (size_t)flen) : 0, dash ? dash : "");
            face_style(fc, style);
            if (fc->weight != 400 || fc->italic) { /* most designs never use it: read it when one does */
                fc->path = strdup(path);
                fc->data = NULL;
                free(data);
            }
            bool known = false;
            for (int i = 0; i < nfamilies; i++)
                known |= !strcasecmp(family_names[i], fc->family);
            if (!known)
                family_names[nfamilies++] = fc->family;
            nfaces++;
        } else {
            free(data);
        }
        fclose(f);
    }
    closedir(d);
}

static bool face_load(struct face *fc) {
    if (fc->data)
        return true;
    FILE *f = fc->path ? fopen(fc->path, "rb") : NULL;
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) == (size_t)size &&
        stbtt_InitFont(&fc->info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        fc->data = data;
    else
        free(data);
    fclose(f);
    if (!fc->data)
        fc->path = NULL; /* don't try again */
    return fc->data != NULL;
}

/* One entry per family, whatever faces it has. */
const char *const *font_families(int *count) {
    *count = nfamilies;
    return family_names;
}

/* The face of `family` nearest to the weight and slant asked for (CSS's
 * matching, simplified): a real italic beats a slanted roman, then the
 * closest weight, heavier first for bold asks and lighter first otherwise. */
static struct face *find_face(const char *family, int weight, bool italic) {
    const char *fam = family;
    bool any = false;
    for (int i = 0; i < nfaces && !any; i++)
        any = !strcasecmp(faces[i].family, fam);
    if (!any)
        fam = "Inter";
    struct face *best = NULL;
    int best_score = 1 << 30;
    for (int i = 0; i < nfaces; i++) {
        struct face *fc = &faces[i];
        if (strcasecmp(fc->family, fam) || (!fc->data && !fc->path))
            continue;
        int dw = fc->weight - weight;
        int score = abs(dw) * 2 + (dw < 0 ? (weight > 500 ? 1 : 0) : (weight > 500 ? 0 : 1));
        if (fc->italic != italic)
            score += 10000;
        if (score < best_score)
            best_score = score, best = fc;
    }
    if (best && !face_load(best))
        return find_face(family, weight, italic); /* that file is gone: the next best */
    return best ? best : nfaces ? &faces[0] : NULL;
}

/* Sizes are kept to quarter pixels so animations that scale text (hover
 * zoom, arrivals) reuse a handful of fonts instead of making one a frame. */
static float quant(float px) { return roundf(px * 4) / 4; }

static void font_drop_glyphs(font *f) {
    for (int i = 0; i < CACHE_GLYPHS; i++) {
        free(f->cache[i].bitmap);
        f->cache[i].bitmap = NULL;
    }
    glyph_bytes -= f->bytes;
    f->bytes = 0;
}

font *font_get_styled(const char *family, float px, int weight, bool italic, float tracking) {
    if (weight <= 0)
        weight = 400;
    if (weight > 1000)
        weight = 1000;
    struct face *face = find_face(family ? family : "Inter", weight, italic);
    if (!face)
        return NULL;
    px = quant(px);
    tracking = roundf(tracking * 4) / 4;
    for (int i = 0; i < nfonts; i++) {
        font *f = &fonts[i];
        if (f->face == face && f->px == px && f->weight == weight && f->italic == italic && f->tracking == tracking) {
            f->used = ++tick;
            return f;
        }
    }
    font *f;
    if (nfonts < MAX_FONTS) {
        f = &fonts[nfonts++];
    } else { /* reuse the one unused the longest */
        f = &fonts[0];
        for (int i = 1; i < MAX_FONTS; i++)
            if (fonts[i].used < f->used)
                f = &fonts[i];
        font_drop_glyphs(f);
    }
    memset(f, 0, sizeof *f);
    f->face = face;
    f->px = px;
    f->weight = weight;
    f->italic = italic;
    f->tracking = tracking;
    f->used = ++tick;
    f->scale = stbtt_ScaleForPixelHeight(&face->info, px);
    stbtt_GetFontVMetrics(&face->info, &f->ascent, &f->descent, &f->line_gap);
    /* What the face lacks is made up: about a twentieth of the size of
     * extra stem for each 300 units of weight, and a shear for italic. */
    if (weight - face->weight >= 100)
        f->bold = px * 0.05f * (float)(weight - face->weight) / 300.0f;
    if (italic && !face->italic)
        f->slant = SLANT;
    return f;
}

font *font_get(const char *family, float px) { return font_get_styled(family, px, 400, false, 0); }

font *font_with(font *base, int weight, bool italic, float tracking) {
    if (!base)
        return NULL;
    if ((weight <= 0 || weight == base->weight) && italic == base->italic && tracking == base->tracking)
        return base;
    return font_get_styled(base->face->family, base->px, weight > 0 ? weight : base->weight, italic, tracking);
}

float font_px(font *f) { return f ? f->px : 0; }

int font_ascent(font *f) { return (int)roundf(f->ascent * f->scale); }

int font_line_height(font *f) { return (int)roundf((f->ascent - f->descent + f->line_gap) * f->scale); }

/* UTF-8 decode; advances *s. */
static int next_cp(const char **s, const char *end) {
    const unsigned char *p = (const unsigned char *)*s;
    int cp = *p++, extra = 0;
    if (cp >= 0xf0)
        cp &= 0x07, extra = 3;
    else if (cp >= 0xe0)
        cp &= 0x0f, extra = 2;
    else if (cp >= 0xc0)
        cp &= 0x1f, extra = 1;
    while (extra-- && (const char *)p < end && (*p & 0xc0) == 0x80)
        cp = cp << 6 | (*p++ & 0x3f);
    *s = (const char *)p;
    return cp;
}

/* Emboldens an outline in place, the way FreeType does: every point moves
 * out along the bisector of its two edges so each edge moves out by (sx, sy)
 * font units. Inner contours turn the other way, so counters shrink. */
static void embolden(stbtt_vertex *v, int nv, float sx, float sy) {
    /* All points of the outline in order, control points included. */
    short *px[1024], *py[1024];
    int start[129], nc = 0, np = 0;
    for (int i = 0; i < nv; i++) {
        if (np >= 1021 || (v[i].type == STBTT_vmove && nc == 128))
            return; /* too intricate to track: left at its own weight rather than half-moved */
        if (v[i].type == STBTT_vmove) {
            start[nc++] = np;
        } else if (v[i].type == STBTT_vcurve || v[i].type == STBTT_vcubic) {
            px[np] = &v[i].cx, py[np++] = &v[i].cy;
            if (v[i].type == STBTT_vcubic)
                px[np] = &v[i].cx1, py[np++] = &v[i].cy1;
        }
        px[np] = &v[i].x, py[np++] = &v[i].y;
    }
    start[nc] = np;
    double area = 0;
    for (int c = 0; c < nc; c++)
        for (int i = start[c]; i < start[c + 1]; i++) {
            int j = i + 1 < start[c + 1] ? i + 1 : start[c];
            area += (double)*px[i] * *py[j] - (double)*px[j] * *py[i];
        }
    float orient = area < 0 ? 1.0f : -1.0f; /* clockwise outlines (TrueType): outward is to the left */
    float nx[1024], ny[1024];
    for (int c = 0; c < nc; c++) {
        int a = start[c], n = start[c + 1] - a;
        if (n < 3)
            continue;
        /* the contour closes on its first point; that point moves once */
        int m = (*px[a + n - 1] == *px[a] && *py[a + n - 1] == *py[a]) ? n - 1 : n;
        for (int i = 0; i < m; i++) {
            float x = *px[a + i], y = *py[a + i];
            int p = i, q = i;
            float ix = 0, iy = 0, ox = 0, oy = 0;
            for (int k = 0; k < m && ix == 0 && iy == 0; k++) {
                p = (p + m - 1) % m;
                ix = x - *px[a + p], iy = y - *py[a + p];
            }
            for (int k = 0; k < m && ox == 0 && oy == 0; k++) {
                q = (q + 1) % m;
                ox = *px[a + q] - x, oy = *py[a + q] - y;
            }
            float li = sqrtf(ix * ix + iy * iy), lo = sqrtf(ox * ox + oy * oy);
            if (li == 0 || lo == 0) {
                nx[a + i] = x, ny[a + i] = y;
                continue;
            }
            ix /= li, iy /= li, ox /= lo, oy /= lo;
            float n1x = -iy * orient, n1y = ix * orient, n2x = -oy * orient, n2y = ox * orient;
            float d = 1 + ix * ox + iy * oy, mx, my;
            if (d < 0.25f) { /* a spike: don't let the miter run away */
                mx = (n1x + n2x) * 0.5f / 0.25f, my = (n1y + n2y) * 0.5f / 0.25f;
                float l = sqrtf(mx * mx + my * my);
                if (l > 2)
                    mx *= 2 / l, my *= 2 / l;
            } else {
                mx = (n1x + n2x) / d, my = (n1y + n2y) / d;
            }
            nx[a + i] = x + mx * sx, ny[a + i] = y + my * sy;
        }
        for (int i = 0; i < m; i++)
            *px[a + i] = (short)lrintf(nx[a + i]), *py[a + i] = (short)lrintf(ny[a + i]);
        if (m < n)
            *px[a + n - 1] = *px[a], *py[a + n - 1] = *py[a];
    }
}

/* A glyph drawn from its outline after emboldening and/or shearing. */
static unsigned char *synth_bitmap(font *f, int gi, int *w, int *h, int *xoff, int *yoff) {
    stbtt_fontinfo *info = &f->face->info;
    stbtt_vertex *v;
    int nv = stbtt_GetGlyphShape(info, gi, &v);
    *w = *h = *xoff = *yoff = 0;
    if (nv <= 0)
        return NULL;
    float sx = f->bold / 2 / f->scale, sy = sx * 0.5f;
    if (f->bold > 0)
        embolden(v, nv, sx, sy);
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < nv; i++) {
        stbtt_vertex *p = &v[i];
        /* keep the left side bearing where it was; lean about the baseline */
        p->x = (short)lrintf(p->x + sx + p->y * f->slant);
        p->cx = (short)lrintf(p->cx + sx + p->cy * f->slant);
        p->cx1 = (short)lrintf(p->cx1 + sx + p->cy1 * f->slant);
        int n = p->type == STBTT_vcubic ? 3 : p->type == STBTT_vcurve ? 2 : 1;
        short xs[3] = {p->x, p->cx, p->cx1}, ys[3] = {p->y, p->cy, p->cy1};
        for (int k = 0; k < n; k++) {
            minx = fminf(minx, xs[k]), maxx = fmaxf(maxx, xs[k]);
            miny = fminf(miny, ys[k]), maxy = fmaxf(maxy, ys[k]);
        }
    }
    int ix0 = (int)floorf(minx * f->scale), iy0 = (int)floorf(-maxy * f->scale);
    int ix1 = (int)ceilf(maxx * f->scale), iy1 = (int)ceilf(-miny * f->scale);
    stbtt__bitmap gbm = {ix1 - ix0, iy1 - iy0, ix1 - ix0, NULL};
    if (gbm.w > 0 && gbm.h > 0 && (gbm.pixels = calloc((size_t)gbm.w * (size_t)gbm.h, 1)))
        stbtt_Rasterize(&gbm, 0.35f, v, nv, f->scale, f->scale, 0, 0, ix0, iy0, 1, info->userdata);
    stbtt_FreeShape(info, v);
    if (!gbm.pixels)
        return NULL;
    *w = gbm.w, *h = gbm.h, *xoff = ix0, *yoff = iy0;
    return gbm.pixels;
}

/* Keeps the glyph cache within budget by emptying the fonts used longest
 * ago (never `keep`, the one drawing now). */
static void trim_glyphs(font *keep) {
    while (glyph_bytes > GLYPH_BUDGET) {
        font *old = NULL;
        for (int i = 0; i < nfonts; i++)
            if (&fonts[i] != keep && fonts[i].bytes && (!old || fonts[i].used < old->used))
                old = &fonts[i];
        if (!old)
            break;
        font_drop_glyphs(old);
    }
}

static struct glyph *glyph(font *f, int cp) {
    struct glyph *g = &f->cache[(unsigned)cp % CACHE_GLYPHS];
    if (g->bitmap && g->cp == cp)
        return g;
    /* Characters the face doesn't have (emoji, mostly) are left out rather
     * than drawn as empty boxes. */
    int gi = stbtt_FindGlyphIndex(&f->face->info, cp);
    if (cp > 0x7f && !gi) {
        static struct glyph none;
        none.cp = cp;
        none.w = none.h = none.advance = 0;
        if (!none.bitmap)
            none.bitmap = calloc(1, 1);
        return &none;
    }
    if (g->bitmap) {
        size_t old = (size_t)g->w * (size_t)g->h;
        f->bytes -= old, glyph_bytes -= old;
        free(g->bitmap);
        g->bitmap = NULL;
    }
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&f->face->info, gi, &adv, &lsb);
    g->cp = cp;
    g->advance = (int)roundf(adv * f->scale + f->bold);
    if (f->bold > 0 || f->slant != 0)
        g->bitmap = synth_bitmap(f, gi, &g->w, &g->h, &g->xoff, &g->yoff);
    else
        g->bitmap = stbtt_GetGlyphBitmap(&f->face->info, f->scale, f->scale, gi, &g->w, &g->h, &g->xoff, &g->yoff);
    if (!g->bitmap) {
        g->bitmap = calloc(1, 1);
        g->w = g->h = 0;
    }
    size_t sz = (size_t)g->w * (size_t)g->h;
    f->bytes += sz, glyph_bytes += sz;
    if (glyph_bytes > GLYPH_BUDGET)
        trim_glyphs(f);
    return g;
}

static int kern(font *f, int prev, int cp) {
    return (int)roundf(stbtt_GetCodepointKernAdvance(&f->face->info, prev, cp) * f->scale + f->tracking);
}

int text_width_n(font *f, const char *s, int n) {
    if (!f)
        return 0;
    const char *end = s + n;
    int w = 0, prev = 0;
    while (s < end && *s) {
        int cp = next_cp(&s, end);
        if (prev)
            w += kern(f, prev, cp);
        w += glyph(f, cp)->advance;
        prev = cp;
    }
    return w;
}

int text_width(font *f, const char *s) { return text_width_n(f, s, (int)strlen(s)); }

int draw_text_n(canvas *c, font *f, int x, int y, const char *s, int n, color col) {
    if (!f)
        return 0;
    f->used = ++tick;
    const char *end = s + n;
    int pen = x, prev = 0, base = y + font_ascent(f);
    while (s < end && *s) {
        int cp = next_cp(&s, end);
        if (prev)
            pen += kern(f, prev, cp);
        struct glyph *g = glyph(f, cp);
        int gx = pen + g->xoff, gy = base + g->yoff;
        for (int j = 0; j < g->h; j++) {
            int py = gy + j;
            if (py < c->cy0 || py >= c->cy1)
                continue;
            for (int i = 0; i < g->w; i++) {
                int px = gx + i;
                if (px < c->cx0 || px >= c->cx1)
                    continue;
                unsigned a = g->bitmap[j * g->w + i];
                if (!a)
                    continue;
                uint32_t *p = &c->px[(size_t)py * (size_t)c->w + (size_t)px];
                uint32_t aa = C_A(col) * a / 255;
                uint32_t ia = 255 - aa;
                *p = 0xff000000u | ((C_R(col) * aa + C_R(*p) * ia) / 255) << 16 |
                     ((C_G(col) * aa + C_G(*p) * ia) / 255) << 8 | ((C_B(col) * aa + C_B(*p) * ia) / 255);
            }
        }
        pen += g->advance;
        prev = cp;
    }
    return pen - x;
}

int draw_text(canvas *c, font *f, int x, int y, const char *s, color col) {
    return draw_text_n(c, f, x, y, s, (int)strlen(s), col);
}

int draw_text_wrapped(canvas *c, font *f, rect box, const char *s, color col, float spacing) {
    return draw_text_box(c, f, box, s, col, spacing, TEXT_LEFT, 0);
}

void draw_text_centered(canvas *c, font *f, rect r, const char *s, color col) {
    if (!f)
        return;
    int w = text_width(f, s), h = font_line_height(f);
    draw_text(c, f, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, s, col);
}
