#define _GNU_SOURCE
#include "app.h"
#include "fm/fm.h"
#include "picture.h"
#include "stylist.h"
#include "wallpaper.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The stylist's eyes: a design drawn offscreen exactly as the desktop
 * would draw it (same engine, same wallpaper, same icons), linted, and
 * photographed for a model that can see. Nothing here talks to a model;
 * stylist.c does that. */

#define SHOT_W 1200 /* pictures for the model: big enough to judge 13px type */
#define SHOT_H 675

/* ---- an app window's stand-in ---- */

void stylist_placeholder_view(const char *app, rect r) {
    (void)app;
    const design *d = &A.d;
    float px = d->body_px >= 12 ? d->body_px : 15;
    font *f = font_get(d->font_body, px), *small = font_get(d->font_body, px - 3 >= 12 ? px - 3 : 12);
    static const char *const names[] = {"Documents", "Pictures", "Music", "Projects", "letter-to-ana.txt",
                                        "recipes.txt", "notes.txt"};
    static const char *const info[] = {"folder", "folder", "folder", "folder", "2 KB", "5 KB", "1 KB"};
    int rowh = (int)(px * 2.5f), y = r.y + 6;
    draw_text(A.frame, small, r.x + 4, y, "/root", d->muted);
    y += (int)(px * 2);
    for (int i = 0; i < 7 && y + rowh <= r.y + r.h; i++, y += rowh) {
        bool dir = i < 4;
        int s = rowh - 14;
        icon_symbol(A.frame, (rect){r.x + 6, y + (rowh - 4 - s) / 2, s, s}, dir ? APP_FILES : APP_NOTES, 1.8f,
                    dir ? d->accent : color_alpha(d->surface_text, 0.6f));
        draw_text(A.frame, f, r.x + 16 + s, y + (rowh - 4 - font_line_height(f)) / 2, names[i], d->surface_text);
        draw_text(A.frame, small, r.x + r.w - 110, y + (rowh - 4 - font_line_height(small)) / 2, info[i], d->muted);
    }
}

/* ---- pictures ---- */

static char *picture_of(const canvas *src) {
    canvas *s = canvas_new(SHOT_W, SHOT_H);
    if (!s)
        return NULL;
    canvas_scale(s, src);
    size_t len = 0;
    unsigned char *jpg = jpeg_encode(s->px, s->w, s->h, s->w, 82, &len);
    canvas_free(s);
    if (!jpg)
        return NULL;
    char *b64 = base64_encode(jpg, len);
    free(jpg);
    return b64;
}

bool stylist_write_jpeg(const char *path, const char *b64) {
    if (!b64)
        return false;
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = b64; *p && *p != '='; p++) {
        const char *k = strchr(tab, *p);
        if (!k)
            continue;
        acc = acc << 6 | (uint32_t)(k - tab);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            fputc((int)((acc >> bits) & 0xff), f);
        }
    }
    fclose(f);
    return true;
}

/* ---- looking at a design ---- */

void stylist_look_at(const char *text, bool pictures, stylist_look *out) {
    memset(out, 0, sizeof *out);
    fm_doc *doc = text ? fm_parse(text) : NULL;
    design d;
    char probs[300] = "";
    if (!doc || !design_from_facet(&d, doc, &A.prof, probs, sizeof probs)) {
        snprintf(out->problems, sizeof out->problems, "%s", probs[0] ? probs : "the document could not be read");
        if (doc)
            fm_free(doc);
        return;
    }
    out->ok = true;
    if (probs[0]) /* small things the reader fixed itself, worth repeating */
        snprintf(out->problems, sizeof out->problems, "%s", probs);
    int W = A.W > 0 ? A.W : 1600, H = A.H > 0 ? A.H : 900;
    canvas *work = canvas_new(W, H), *wall = canvas_new(W, H);
    if (!work || !wall) {
        canvas_free(work);
        canvas_free(wall);
        fm_free(doc);
        return;
    }
    /* The host's callbacks (icons, labels, the orb) draw with the shell's
     * own state: point it at this design and this canvas for a moment. */
    canvas *saved_frame = A.frame;
    fm_doc *saved_ui = A.ui;
    input_state saved_in = A.in;
    static design saved_d;
    saved_d = A.d;
    A.d = d;
    A.ui = doc;
    memset(&A.in, 0, sizeof A.in);
    A.in.mx = A.in.my = -10000;
    A.frame = work;

    wallpaper_paint(wall, &d);
    fm_lint_opts o = {work, wall, {0, 0, W, H}, A.prof.high_contrast ? 7.0f : 4.5f, APP_COUNT};
    fm_lint_run(doc, &o, &out->lint);
    if (out->problems[0] && out->lint.score == 0)
        out->lint.score = 1;
    if (pictures) {
        fm_lint_paint(doc, &o, NULL, false);
        out->jpeg_b64[0] = picture_of(work);
        snprintf(out->shows[0], sizeof out->shows[0], "the desktop at rest, nothing open");
        fm_node *pop = fm_lint_main_popover(doc);
        fm_lint_paint(doc, &o, pop, true);
        out->jpeg_b64[1] = picture_of(work);
        if (pop)
            snprintf(out->shows[1], sizeof out->shows[1], "#%s open, over an app window drawn with your template",
                     pop->id);
        else
            snprintf(out->shows[1], sizeof out->shows[1], "an app window drawn with your template");
        out->npictures = (out->jpeg_b64[0] != NULL) + (out->jpeg_b64[1] != NULL);
    }

    A.frame = saved_frame;
    A.ui = saved_ui;
    A.in = saved_in;
    A.d = saved_d;
    canvas_free(work);
    canvas_free(wall);
    fm_free(doc);
}

void stylist_look_free(stylist_look *l) {
    for (int i = 0; i < 2; i++) {
        free(l->jpeg_b64[i]);
        l->jpeg_b64[i] = NULL;
    }
}

/* ---- starter themes ---- */

/* OS_THEMES points the build machine's tools at the source tree. */
static const char *themes_dir(void) {
    const char *e = getenv("OS_THEMES");
    return e && *e ? e : STYLIST_THEMES;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    char *s = malloc((size_t)n + 1);
    size_t got = fread(s, 1, (size_t)n, f);
    s[got] = 0;
    fclose(f);
    return s;
}

/* One top-level element of a document, opening tag to closing tag (or
 * the self-closing tag). NULL if absent. */
static const char *section(const char *doc, const char *tag, size_t *len) {
    size_t tl = strlen(tag);
    for (const char *p = doc; (p = strchr(p, '<')); p++) {
        if (strncasecmp(p + 1, tag, tl) || !(p[1 + tl] == ' ' || p[1 + tl] == '>' || p[1 + tl] == '/' ||
                                              p[1 + tl] == '\n' || p[1 + tl] == '\t'))
            continue;
        const char *gt = strchr(p, '>');
        if (!gt)
            return NULL;
        if (gt[-1] == '/') {
            *len = (size_t)(gt + 1 - p);
            return p;
        }
        char close[32];
        snprintf(close, sizeof close, "</%s>", tag);
        const char *e = strcasestr(gt, close);
        *len = e ? (size_t)(e + strlen(close) - p) : strlen(p);
        return p;
    }
    return NULL;
}

/* The text between a section's tags. */
static void inner(const char *sec, size_t len, const char **s, size_t *n) {
    const char *gt = memchr(sec, '>', len);
    const char *end = sec + len;
    *s = gt ? gt + 1 : sec;
    while (end > *s && end[-1] != '<')
        end--;
    *n = end > *s ? (size_t)(end - 1 - *s) : 0;
}

bool stylist_quiet;

char *stylist_expand_base(const char *reply) {
    const char *s = reply ? strcasestr(reply, "<facet") : NULL;
    if (!s)
        return NULL;
    const char *e = strcasestr(s, "</facet>");
    char *doc = strndup(s, e ? (size_t)(e + 8 - s) : strlen(s));
    const char *gt = strchr(doc, '>');
    const char *b = strcasestr(doc, "base=");
    if (!gt || !b || b > gt)
        return doc;
    b += 5;
    if (*b == '"' || *b == '\'')
        b++;
    char name[48];
    int k = 0;
    while (k < 47 && (isalnum((unsigned char)b[k]) || b[k] == '-' || b[k] == '_'))
        name[k] = (char)tolower((unsigned char)b[k]), k++;
    name[k] = 0;
    char path[160];
    snprintf(path, sizeof path, "%s/%s.xml", themes_dir(), name);
    char *theme = k ? read_file(path) : NULL;
    if (!theme) {
        if (!stylist_quiet)
            LOG("stylist: no theme \"%s\" to start from; using the reply as it is\n", name);
        return doc;
    }
    sbuf o;
    sb_init(&o);
    sb_add(&o, "<facet>\n");
    static const char *const tags[] = {"meta", "apps", "wallpaper", "style", "desktop", "template"};
    for (int i = 0; i < 6; i++) {
        size_t la = 0, lb = 0;
        const char *a = section(doc + (gt - doc), tags[i], &la), *t = section(theme, tags[i], &lb);
        if (!strcmp(tags[i], "style")) {
            /* the theme's rules, then theirs: later rules and variables win */
            const char *x;
            size_t n;
            sb_add(&o, "<style>\n");
            if (t) {
                inner(t, lb, &x, &n);
                sb_addn(&o, x, n);
                sb_add(&o, "\n");
            }
            if (a) {
                inner(a, la, &x, &n);
                sb_addn(&o, x, n);
                sb_add(&o, "\n");
            }
            sb_add(&o, "</style>\n");
        } else if (a || t) {
            sb_addn(&o, a ? a : t, a ? la : lb);
            sb_add(&o, "\n");
        }
    }
    sb_add(&o, "</facet>\n");
    free(theme);
    free(doc);
    if (!stylist_quiet)
        LOG("stylist: started from the theme \"%s\"\n", name);
    return o.p;
}

static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void stylist_theme_menu(sbuf *b) {
    DIR *dir = opendir(themes_dir());
    if (!dir)
        return;
    char *names[24];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) && n < 24) {
        size_t l = strlen(de->d_name);
        if (l > 4 && !strcmp(de->d_name + l - 4, ".xml"))
            names[n++] = strndup(de->d_name, l - 4);
    }
    closedir(dir);
    if (!n)
        return;
    qsort(names, (size_t)n, sizeof names[0], by_name);
    sb_add(b, "\nSTARTING POINTS: finished, tested desktops you may start from instead of a blank page, when one "
              "fits them:\n");
    for (int i = 0; i < n; i++) {
        char path[160], about[400] = "";
        snprintf(path, sizeof path, "%s/%s.xml", themes_dir(), names[i]);
        char *t = read_file(path);
        const char *a = t ? strstr(t, "theme-about=\"") : NULL;
        if (a) {
            a += 13;
            snprintf(about, sizeof about, "%.*s", (int)strcspn(a, "\""), a);
        }
        sb_printf(b, "- %s: %s\n", names[i], about[0] ? about : "a starting point");
        free(t);
        free(names[i]);
    }
    sb_add(b, "To start from one, write <facet base=\"name\"> and give only what changes: always <meta> and <apps> "
              "(their names and labels), then <wallpaper> if you change it, a <style> whose rules are added after "
              "the theme's, and a whole <desktop> or <template id=\"window\"> only if you rewrite it. Every theme "
              "takes its colours, fonts and corners from :root variables (--bg --surface --surface-2 --text "
              "--muted --accent --accent2 --font-heading --font-body --radius), so redefining them in :root "
              "recolours all of it. Make it theirs: their palette, their words, their wallpaper; a theme left "
              "as it is is not a design. Writing your own from nothing is just as welcome.\n");
}
