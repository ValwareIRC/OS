#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

#include "iconart.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* Linux apps' own icons. An app installed under /apps has an id and the
 * command that starts it; its .desktop file under /usr/share/applications
 * (matched by file name, Exec= or StartupWMClass=) names its icon, which is
 * looked for in the hicolor theme at the nearest size at or above the one
 * wanted, then in /usr/share/pixmaps. Only PNGs are read: an app with only
 * an SVG keeps the monogram tile. Found paths and scaled pictures are
 * cached, so the disk is searched once per app. */

#define APPS_DIR "/usr/share/applications"
#include "sys.h" /* FLATPAK_*_EXPORTS */

struct found {
    char id[96];
    char icon[200]; /* Icon= value (a name or a path); "" = none */
};
static struct found found[16];
static int nfound;

static const char *base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* First word of an Exec= line, without its directory. */
static void exec_name(const char *exec, char *out, size_t n) {
    while (*exec == ' ' || *exec == '"')
        exec++;
    size_t k = strcspn(exec, " \"");
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%.*s", (int)(k < sizeof tmp ? k : sizeof tmp - 1), exec);
    snprintf(out, n, "%s", base(tmp));
}

static bool desktop_icon_in(const char *apps_dir, const char *id, const char *exec, char *icon, size_t n);

/* The system's apps, then those installed with Flatpak. */
static bool desktop_icon(const char *id, const char *exec, char *icon, size_t n) {
    return desktop_icon_in(APPS_DIR, id, exec, icon, n) || desktop_icon_in(FLATPAK_DISK_EXPORTS "/applications", id, exec, icon, n) ||
           desktop_icon_in(FLATPAK_SYSTEM_EXPORTS "/applications", id, exec, icon, n);
}

static bool desktop_icon_in(const char *apps_dir, const char *id, const char *exec, char *icon, size_t n) {
    char want_exec[96] = "";
    if (exec && *exec)
        exec_name(exec, want_exec, sizeof want_exec);
    DIR *d = opendir(apps_dir);
    if (!d)
        return false;
    struct dirent *e;
    bool ok = false;
    while (!ok && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 9 || strcmp(e->d_name + len - 8, ".desktop"))
            continue;
        char path[512], line[512], ic[200] = "", ex[96] = "", wm[64] = "";
        snprintf(path, sizeof path, "%s/%s", apps_dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        bool main_group = false;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '[') { /* only the [Desktop Entry] group; actions have their own icons */
                main_group = !strcmp(line, "[Desktop Entry]");
                continue;
            }
            if (!main_group)
                continue;
            if (!strncmp(line, "Icon=", 5))
                snprintf(ic, sizeof ic, "%s", line + 5);
            else if (!strncmp(line, "Exec=", 5))
                exec_name(line + 5, ex, sizeof ex);
            else if (!strncmp(line, "StartupWMClass=", 15))
                snprintf(wm, sizeof wm, "%s", line + 15);
        }
        fclose(f);
        bool match = (!strncasecmp(e->d_name, id, len - 8) && strlen(id) == len - 8) ||
                     (want_exec[0] && ex[0] && !strcmp(want_exec, ex)) || (wm[0] && !strcasecmp(wm, id));
        if (match && ic[0]) {
            snprintf(icon, n, "%s", ic);
            ok = true;
        }
    }
    closedir(d);
    return ok;
}

static const char *icon_name_for(const char *id, const char *exec) {
    for (int i = 0; i < nfound; i++)
        if (!strcmp(found[i].id, id))
            return found[i].icon;
    struct found *f = &found[nfound < 16 ? nfound++ : 15];
    snprintf(f->id, sizeof f->id, "%s", id);
    f->icon[0] = 0;
    char own[300];
    snprintf(own, sizeof own, "/apps/%s/icon.png", id);
    if (access(own, R_OK) == 0)
        snprintf(f->icon, sizeof f->icon, "%s", own);
    else if (!desktop_icon(id, exec, f->icon, sizeof f->icon)) {
        /* not installed yet: the icon Flathub's catalog brings for it */
        snprintf(f->icon, sizeof f->icon, "/var/lib/os-data/flatpak/appstream/flathub/x86_64/active/icons/128x128/%s.png", id);
        if (access(f->icon, R_OK) != 0)
            snprintf(f->icon, sizeof f->icon, "/var/lib/flatpak/appstream/flathub/x86_64/active/icons/128x128/%s.png", id);
        if (access(f->icon, R_OK) != 0)
            f->icon[0] = 0;
    }
    return f->icon;
}

/* The file for an icon name at about `size` px, or false. */
static bool icon_file(const char *name, int size, char *out, size_t n) {
    if (!name[0])
        return false;
    if (name[0] == '/') {
        size_t l = strlen(name);
        if (l < 4 || strcasecmp(name + l - 4, ".png") || access(name, R_OK))
            return false;
        snprintf(out, n, "%s", name);
        return true;
    }
    static const int sizes[] = {16, 22, 24, 32, 36, 48, 64, 72, 96, 128, 192, 256, 512};
    const int ns = (int)(sizeof sizes / sizeof sizes[0]);
    int first = 0;
    while (first < ns - 1 && sizes[first] < size)
        first++;
    /* the nearest at or above the size wanted, then smaller ones */
    static const char *const roots[] = {"/usr/share/icons/hicolor", FLATPAK_DISK_EXPORTS "/icons/hicolor",
                                        FLATPAK_SYSTEM_EXPORTS "/icons/hicolor"};
    for (int pass = 0; pass < 2; pass++)
        for (int i = pass ? first - 1 : first; pass ? i >= 0 : i < ns; i += pass ? -1 : 1)
            for (int r = 0; r < 3; r++) {
                snprintf(out, n, "%s/%dx%d/apps/%s.png", roots[r], sizes[i], sizes[i], name);
                if (access(out, R_OK) == 0)
                    return true;
            }
    snprintf(out, n, "/usr/share/pixmaps/%s.png", name);
    return access(out, R_OK) == 0;
}

/* ---- scaled pictures ---- */

struct pic {
    char id[96];
    int n, tile, look;
    color tint;
    unsigned used;
    uint32_t *px; /* n*n premultiplied ARGB; NULL = nothing to draw */
};
#define PICS 32
static struct pic pics[PICS];
static unsigned tick;

static float clampf(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* Coverage of a rounded square of side n with corner radius rad at (x, y). */
static float round_mask(float x, float y, float n, float rad) {
    float h = n / 2, qx = fabsf(x - h) - h + rad, qy = fabsf(y - h) - h + rad;
    float d = hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - rad;
    return clampf(0.5f - d);
}

/* Scales an RGBA picture into an n*n premultiplied square, keeping its
 * proportions (averaging every source pixel that falls in a target one). */
static uint32_t *scale_icon(const unsigned char *src, int w, int h, int n, int tile, enum icon_look look, color tint) {
    uint32_t *out = calloc((size_t)n * (size_t)n, 4);
    if (!out)
        return NULL;
    /* Pictures that fill their whole square (mostly solid round the edge)
     * get the tile's rounded corners, so they sit with the other icons. */
    int edge = 0, solid = 0;
    for (int i = 0; i < w; i++)
        for (int k = 0; k < 2; k++, edge++)
            solid += src[((size_t)(k ? h - 1 : 0) * (size_t)w + (size_t)i) * 4 + 3] > 200;
    for (int j = 0; j < h; j++)
        for (int k = 0; k < 2; k++, edge++)
            solid += src[((size_t)j * (size_t)w + (size_t)(k ? w - 1 : 0)) * 4 + 3] > 200;
    bool opaque = solid * 10 > edge * 6;
    float m = (float)(w > h ? w : h);
    float offx = (m - (float)w) / 2, offy = (m - (float)h) / 2;
    float inset = opaque ? 0.06f * n : 0; /* a masked square is drawn a little smaller, like a tile */
    float span = n - 2 * inset, kk = m / span;
    float rad = tile == 1 ? span / 2 : tile == 3 ? span * 0.12f : span * 0.225f;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float sx0 = (i - inset) * kk - offx, sy0 = (j - inset) * kk - offy;
            float sx1 = sx0 + kk, sy1 = sy0 + kk;
            int x0 = (int)floorf(sx0), y0 = (int)floorf(sy0), x1 = (int)ceilf(sx1), y1 = (int)ceilf(sy1);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (y1 <= y0)
                y1 = y0 + 1;
            float r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    cnt++;
                    if (x < 0 || y < 0 || x >= w || y >= h)
                        continue;
                    const unsigned char *p = &src[((size_t)y * (size_t)w + (size_t)x) * 4];
                    float pa = p[3] / 255.0f;
                    r += p[0] * pa, g += p[1] * pa, b += p[2] * pa, a += pa;
                }
            if (cnt <= 0 || a <= 0)
                continue;
            r /= cnt, g /= cnt, b /= cnt, a /= cnt;
            if (opaque) {
                float mk = round_mask(i + 0.5f - inset, j + 0.5f - inset, span, rad);
                r *= mk, g *= mk, b *= mk, a *= mk;
            }
            if (look == LOOK_MONO && a > 0) { /* in the tint, keeping how light each part is */
                float l = (0.299f * r + 0.587f * g + 0.114f * b) / (255 * a);
                color away = color_luma(tint) < 0.4f ? RGBA(255, 255, 255, 255) : RGBA(0, 0, 0, 255);
                color c = color_mix(tint, away, clampf(l) * 0.62f);
                r = C_R(c) * a, g = C_G(c) * a, b = C_B(c) * a;
            }
            int ia = (int)lroundf(clampf(a) * 255);
            int ir = (int)lroundf(r), ig = (int)lroundf(g), ib = (int)lroundf(b);
            out[(size_t)j * (size_t)n + (size_t)i] = RGBA(ir > ia ? ia : ir, ig > ia ? ia : ig, ib > ia ? ia : ib, ia);
        }
    return out;
}

static struct pic *get_pic(const char *id, const char *exec, int n, int tile, enum icon_look look, color tint) {
    if (look != LOOK_MONO)
        tint = 0;
    struct pic *slot = NULL;
    for (int i = 0; i < PICS; i++) {
        struct pic *p = &pics[i];
        if (p->used && !strcmp(p->id, id) && p->n == n && p->tile == tile && p->look == (int)look && p->tint == tint) {
            p->used = ++tick;
            return p;
        }
        if (!slot || (slot->used && (!p->used || p->used < slot->used)))
            slot = p;
    }
    free(slot->px);
    memset(slot, 0, sizeof *slot);
    snprintf(slot->id, sizeof slot->id, "%s", id);
    slot->n = n, slot->tile = tile, slot->look = (int)look, slot->tint = tint, slot->used = ++tick;
    char path[400];
    if (icon_file(icon_name_for(id, exec), n, path, sizeof path)) {
        int w, h, ch;
        unsigned char *img = stbi_load(path, &w, &h, &ch, 4);
        if (img && w > 0 && h > 0)
            slot->px = scale_icon(img, w, h, n, tile, look, tint);
        stbi_image_free(img);
    }
    return slot;
}

bool app_icon_draw(canvas *c, rect r, const char *id, const char *exec, int tile, enum icon_look look, color tint,
                   float opacity, bool hover) {
    int n = r.w < r.h ? r.w : r.h;
    if (!c || !id || !*id || n < 4)
        return false;
    if (n > 512)
        n = 512;
    tint |= 0xff000000u;
    struct pic *p = get_pic(id, exec, n, tile, look, tint);
    if (!p->px)
        return false;
    uint32_t o = (uint32_t)lroundf(clampf(opacity) * 256);
    int x = r.x + (r.w - n) / 2, y = r.y + (r.h - n) / 2;
    for (int j = 0; j < n && o; j++) {
        int py = y + j;
        if (py < c->cy0 || py >= c->cy1)
            continue;
        for (int i = 0; i < n; i++) {
            int qx = x + i;
            if (qx < c->cx0 || qx >= c->cx1)
                continue;
            uint32_t s = p->px[(size_t)j * (size_t)n + (size_t)i], a = C_A(s) * o >> 8;
            if (!a)
                continue;
            uint32_t rr = C_R(s) * o >> 8, g = C_G(s) * o >> 8, b = C_B(s) * o >> 8;
            if (hover)
                rr += (a - rr) / 10, g += (a - g) / 10, b += (a - b) / 10;
            uint32_t *d = &c->px[(size_t)py * (size_t)c->w + (size_t)qx], ia = 255 - a;
            *d = 0xff000000u | (rr + C_R(*d) * ia / 255) << 16 | (g + C_G(*d) * ia / 255) << 8 | (b + C_B(*d) * ia / 255);
        }
    }
    return true;
}
