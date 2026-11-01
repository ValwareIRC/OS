/* A Facet 2 app pictured on the build machine, in a design, as its window
 * would show it: tests/fx_shot.sh APP.facet DESIGN.xml OUT.png [HOME]
 * HOME: a folder used as ~ (the app's files). Inputs and checkboxes are
 * drawn as plain boxes with their text (the shell draws the real ones). */
#define main fm_shot_main
#include "fm_shot.c"
#undef main

#include "../userland/shell/design.h"
#include "../userland/shell/fx/fx_rt.h"
#include "../userland/shell/fx/markup.h"

#include <dirent.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"


static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (fread(s, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    s[n] = 0;
    fclose(f);
    return s;
}
static char *t_read(void *c, const char *p) { (void)c; return slurp_file(p); }
static fx_val t_files(void *c, const char *dir, const char *ending) {
    (void)c;
    fx_val l = fx_list();
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t nl = strlen(e->d_name);
        if (ending && *ending && (nl < strlen(ending) || strcmp(e->d_name + nl - strlen(ending), ending)))
            continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st))
            continue;
        fx_val r = fx_rec();
        fx_rec_set(r, "name", fx_text(e->d_name));
        fx_rec_set(r, "path", fx_text(path));
        fx_rec_set(r, "size", fx_num((double)st.st_size));
        fx_rec_set(r, "modified", fx_num((double)st.st_mtime * 1000));
        fx_rec_set(r, "dir", fx_bool(S_ISDIR(st.st_mode)));
        fx_list_push(l, r);
    }
    if (d)
        closedir(d);
    return l;
}
static fx_val t_apps(void *c) {
    (void)c;
    static const char *const ids[][2] = {{"files", "Files"}, {"notes", "Notes"}, {"settings", "Settings"},
                                         {"terminal", "Terminal"}, {"prism", "Prism"}, {"chromium", "Chromium"}};
    fx_val l = fx_list();
    for (int i = 0; i < 6; i++) {
        fx_val r = fx_rec();
        fx_rec_set(r, "id", fx_text(ids[i][0]));
        fx_rec_set(r, "name", fx_text(ids[i][1]));
        fx_list_push(l, r);
    }
    return l;
}
static double t_now(void *c) { (void)c; return 1793500000000.0 + 83500; }
static void t_error(void *c, const char *m) { (void)c; printf("fault: %s\n", m); }

static fm_node *by_fxin(fm_node *n, int k) {
    for (; n; n = n->next) {
        if (n->kind != FM_ELEM)
            continue;
        const char *v = fm_attr_get(n, "fxin");
        if (v && atoi(v) == k)
            return n;
        fm_node *f = by_fxin(n->first, k);
        if (f)
            return f;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: fx_shot APP.facet DESIGN.xml OUT.png [HOME]\n");
        return 2;
    }
    if (argc > 4)
        setenv("HOME", argv[4], 1);
    fonts_init("build/ports/fonts");
    A.W = 1600, A.H = 900;
    A.frame = canvas_new(A.W, A.H);
    /* the person's design */
    char *dsrc = slurp_file(argv[2]);
    if (!dsrc)
        return 1;
    char *expanded = stylist_expand_base(dsrc);
    fm_doc *ddoc = fm_parse(expanded ? expanded : dsrc);
    char err[200] = "";
    if (!design_from_facet(&A.d, ddoc, &A.prof, err, (int)sizeof err)) {
        printf("design: %s\n", err);
        return 1;
    }
    /* the app */
    char *src = slurp_file(argv[1]);
    fx_errors errs = {0};
    errs.file = argv[1];
    fx_app *app = fx_parse(src, &errs);
    fx_check(app, &errs);
    if (errs.n) {
        fx_print_errors(&errs, stdout);
        return 1;
    }
    fx_host h = {0};
    h.read = t_read, h.files = t_files, h.apps = t_apps, h.now_ms = t_now, h.error = t_error;
    fx_rt *rt = fx_start(app, &h);
    fx_out *view = fx_render(rt);
    fx_out **acts;
    int nacts;
    char *markup = fx_markup(view, app, &A.d, &acts, &nacts);
    fm_doc *doc = fm_parse(markup);
    for (int i = 0; i < doc->nproblems; i++)
        printf("engine: line %d: %s\n", doc->problems[i].line, doc->problems[i].msg);
    /* the desktop's colour behind, a window in the middle */
    fill_vgradient(A.frame, (rect){0, 0, A.W, A.H}, A.d.bg, color_mix(A.d.bg, A.d.accent, 0.25f));
    rect win = {(A.W - 760) / 2, (A.H - 560) / 2, 760, 560};
    shadow_round(A.frame, (rect){win.x, win.y + 16, win.w, win.h}, 14, 40, RGBA(0, 0, 0, 110));
    fill_round(A.frame, win, 14, A.d.surface | 0xff000000u);
    font *tf = font_get(A.d.font_heading, 16);
    draw_text(A.frame, tf, win.x + 18, win.y + 12, app->name, A.d.surface_text);
    rect body = {win.x + 4, win.y + 44, win.w - 8, win.h - 48};
    static input_state none = {.mx = -10000, .my = -10000};
    doc->in = &none;
    canvas_clip(A.frame, body);
    fm_layout(doc, body);
    fm_render_layer(doc, A.frame, 0);
    font *bf = font_get(A.d.font_body, A.d.body_px);
    for (int k = 0; k < nacts; k++) {
        fx_out *o = acts[k];
        if (strcmp(o->tag, "input") && strcmp(o->tag, "field") && strcmp(o->tag, "checkbox"))
            continue;
        fm_node *n = by_fxin(doc->root, k);
        if (!n)
            continue;
        rect r = n->vis;
        fill_round(A.frame, r, 6, color_mix(A.d.surface, A.d.surface_text, 0.07f));
        stroke_round(A.frame, r, 6, 1, color_alpha(A.d.surface_text, 0.2f));
        const char *v = fx_get(o, "value"), *ph = fx_get(o, "placeholder");
        if (!strcmp(o->tag, "checkbox")) {
            const char *c = fx_get(o, "checked");
            if (c && !strcmp(c, "true"))
                fill_round(A.frame, rect_inset(r, 4), 3, A.d.accent);
        } else if (v && *v) {
            draw_text_wrapped(A.frame, bf, rect_inset(r, 9), v, A.d.surface_text, 1.35f);
        } else if (ph) {
            draw_text(A.frame, bf, r.x + 10, r.y + (r.h - font_line_height(bf)) / 2, ph,
                      color_alpha(A.d.surface_text, 0.45f));
        }
    }
    canvas_unclip(A.frame);
    /* only the window, as PNG */
    unsigned char *rgb = malloc((size_t)win.w * win.h * 3);
    for (int y = 0; y < win.h; y++)
        for (int x = 0; x < win.w; x++) {
            uint32_t p = A.frame->px[(size_t)(win.y + y) * A.W + win.x + x];
            unsigned char *o = rgb + ((size_t)y * win.w + x) * 3;
            o[0] = C_R(p), o[1] = C_G(p), o[2] = C_B(p);
        }
    stbi_write_png(argv[3], win.w, win.h, 3, rgb, win.w * 3);
    printf("%s: %d elements, %d clickable or typed into -> %s\n", argv[1], view->nkids, nacts, argv[3]);
    return 0;
}
