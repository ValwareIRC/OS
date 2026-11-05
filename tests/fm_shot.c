/* The stylist's eyes on the build machine: lints a Facet design and saves
 * the pictures a seeing model would get (the desktop at rest; its menu
 * open over a window), drawn by the real engine, wallpaper and icons.
 *   tests/lint_design.sh tests/designs/xp.xml [OUT_DIR]  */
#define _GNU_SOURCE
#include "../userland/shell/app.h"
#include "../userland/shell/fm/fm.h"
#include "../userland/shell/stylist.h"
#include "../userland/shell/menufill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

app A;

/* The bundled apps the VM image carries (/apps), so lists look the same. */
static const char *const extra_ids[] = {"flower", "prism", "smoke"};
static const char *const extra_names[] = {"Flower", "Prism", "Smoke"};
#define NEXTRA 3

int fm_host_app_count(void) { return APP_COUNT + NEXTRA; }
const char *fm_host_app_id(int a) {
    if (a >= 0 && a < APP_COUNT)
        return app_id(a);
    return a >= APP_COUNT && a < APP_COUNT + NEXTRA ? extra_ids[a - APP_COUNT] : "";
}
const char *fm_host_app_label(int a) {
    if (a >= 0 && a < APP_COUNT)
        return A.d.apps[a].label[0] ? A.d.apps[a].label : app_default_name(a);
    if (a < APP_COUNT || a >= APP_COUNT + NEXTRA)
        return "";
    if (A.ui && A.ui->root)
        for (fm_node *s = A.ui->root->first; s; s = s->next)
            if (s->kind == FM_ELEM && !strcmp(s->tag, "apps"))
                for (fm_node *e = s->first; e; e = e->next) {
                    const char *id = fm_attr_get(e, "id"), *l = fm_attr_get(e, "label");
                    if (e->kind == FM_ELEM && id && l && *l && !strcasecmp(id, extra_ids[a - APP_COUNT]))
                        return l;
                }
    return extra_names[a - APP_COUNT];
}
int fm_host_window_count(void) { return 0; }
int fm_host_window_app(int i) { (void)i; return 0; }
const char *fm_host_window_title(int i) { (void)i; return ""; }
bool fm_host_window_focused(int i) { (void)i; return false; }
void fm_host_action(fm_doc *d, fm_node *src, const char *action) { (void)d, (void)src, (void)action; }
bool fm_host_bind(const char *key, char *out, size_t n) {
    if (!strcmp(key, "user.name")) snprintf(out, n, "%s", A.d.user_name);
    else if (!strcmp(key, "ai.name")) snprintf(out, n, "%s", A.d.ai_name);
    else if (!strcmp(key, "greeting")) snprintf(out, n, "%s", A.d.greeting);
    else if (!strcmp(key, "tagline")) snprintf(out, n, "%s", A.d.tagline);
    else if (!strcmp(key, "rationale")) snprintf(out, n, "%s", A.d.rationale);
    else if (!strncmp(key, "window.title", 12)) snprintf(out, n, "%s", fm_host_app_label(APP_FILES));
    else return false;
    return true;
}
void fm_host_draw_view(const char *app, rect r) { stylist_placeholder_view(app, r); }
/* its spot: empty, as on a desktop where it has no look yet (there are no stock ones) */
bool fm_host_wallpaper_range(rect r, color *dark, color *light) { (void)r, (void)dark, (void)light; return false; }
void fm_host_draw_orb(rect r, bool active) { (void)r, (void)active; }
/* its portrait: a stand-in panel here (the figure draws only in the OS) */
void fm_host_draw_avatar(rect r, const char *frame, const char *tint, const char *shot, bool active) {
    (void)frame, (void)tint, (void)shot, (void)active;
    fill_rect(A.frame, r, color_mix(A.d.bg, A.d.accent, 0.25f));
}
/* desktop.c's icons, without the bundled-app tiles' finer points */
void fm_host_draw_icon(rect r, int app, const fm_style *st, bool hover) {
    design dd = A.d;
    if (app >= APP_COUNT) {
        fill_round_vgradient(A.frame, r, r.w * 0.28f, color_mix(dd.accent, 0xffffffffu, 0.2f), dd.accent);
        char letter[2] = {(char)(fm_host_app_label(app)[0] & ~0x20), 0};
        draw_text_centered(A.frame, font_get(dd.font_heading, r.w * 0.48f), r, letter, readable_on(dd.accent));
        return;
    }
    if (!strcasecmp(st->icon_shape, "circle")) dd.icons = ICON_CIRCLE;
    else if (!strcasecmp(st->icon_shape, "squircle")) dd.icons = ICON_SQUIRCLE;
    else if (!strcasecmp(st->icon_shape, "square")) dd.icons = ICON_SQUARE;
    else if (!strcasecmp(st->icon_shape, "none") || !strcasecmp(st->icon_shape, "glyph")) dd.icons = ICON_GLYPH;
    if (!strcasecmp(st->icon_style, "filled")) dd.icon_style = ICONSTYLE_FILLED;
    else if (!strcasecmp(st->icon_style, "line")) dd.icon_style = ICONSTYLE_LINE;
    else if (!strcasecmp(st->icon_style, "glow")) dd.icon_style = ICONSTYLE_GLOW;
    else if (!strcasecmp(st->icon_style, "duotone")) dd.icon_style = ICONSTYLE_DUOTONE;
    ui_icon(A.frame, r, &dd, app, hover);
}
font *fm_host_font(const char *family, float px) { return font_get(family, px < 12 ? 12 : px); }
color fm_host_wallpaper_tone(void) { return A.d.bg; }
uint64_t fm_host_time_ms(void) { return 0; }
const char *clipboard_text(void) { return ""; }
void clipboard_set(const char *t) { (void)t; }
const char *home_dir(void) { return "/home/user"; } /* (stylist_look.c: where a placeholder view reads from) */
/* (the desktop's own timings and backdrop cache: nothing here) */
fm_doc *fm_host_desktop_doc(void) { return NULL; }
uint64_t fm_host_backdrop_sig(void) { return 0; }
uint64_t prof_now_us(void) { return 0; }
void prof_add(const char *name, uint64_t us) { (void)name, (void)us; }

/* every action placed in a menu, and how many twice (menu_shots.sh) */
static void count_placed(const fm_node *n, bool *seen, int ns, int *placed, int *twice) {
    for (const fm_node *c = n->first; c; c = c->next) {
        if (c->kind != FM_ELEM)
            continue;
        const char *w = fm_attr_get(c, "what");
        if (w && !strncmp(w, "menu:", 5)) {
            int i = atoi(w + 5);
            if (i >= 0 && i < ns) {
                *twice += seen[i];
                *placed += !seen[i];
                seen[i] = true;
            }
        }
        count_placed(c, seen, ns, placed, twice);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fm_shot DESIGN.xml [OUT_DIR] [--fonts=DIR]\n");
        return 2;
    }
    const char *fonts = "build/ports/fonts", *outdir = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "--fonts=", 8))
            fonts = argv[i] + 8;
        else
            outdir = argv[i];
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    static char buf[1 << 20];
    buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
    fclose(f);
    fonts_init(fonts);
    A.W = 1600, A.H = 900;
    A.frame = canvas_new(A.W, A.H);
    char *doc = stylist_expand_base(buf);
    /* MENU=kind: that kind's right-click menu, filled as the desktop fills it (menufill.c), shown open (FOCUS) */
    const char *mk = getenv("MENU");
    if (mk && *mk) {
        fm_doc *d = fm_parse(doc ? doc : buf);
        fm_layout(d, (rect){0, 0, A.W, A.H});
        static mf_spec s;
        memset(&s, 0, sizeof s);
        snprintf(s.kind, sizeof s.kind, "%s", mk);
        s.x = A.W * 45 / 100, s.y = A.H * 40 / 100, s.tx = s.x - 60, s.ty = s.y - 30, s.tw = 120, s.th = 60;
        static const struct {
            const char *kind, *ids;
        } sample[] = {
            {"app", "open:Open Notes|close-windows:Close its window|app-info:About this app|uninstall:Uninstall Notes"},
            {"window", "minimize:Minimize|maximize:Maximize|snap-left:Left half|snap-right:Right half|"
                       "keep-on-top:Keep on top|close:Close"},
            {"desktop", "change-wallpaper:Change the wallpaper|new-note:New note|redesign:Redesign|settings:Settings"},
            {"text", "undo:Undo|cut:Cut|copy:Copy|paste:Paste|select-all:Select all|search-for-it:Search for it|"
                     "look-it-up:Look it up"},
            {"image", "open-picture:Open picture|copy-image:Copy image|save-image:Save image|"
                      "set-as-wallpaper:Set as wallpaper"},
            {"file", "open:Open|open-with:Open with|rename:Rename|copy:Copy|cut:Cut|copy-path:Copy path|"
                     "compress:Compress|properties:Properties|move-to-trash:Move to Trash"},
            {"link", "open-link:Open link|copy-link:Copy link"},
        };
        for (size_t k = 0; k < sizeof sample / sizeof sample[0]; k++) {
            if (strcmp(sample[k].kind, mk))
                continue;
            char list[600];
            snprintf(list, sizeof list, "%s", sample[k].ids);
            for (char *save = NULL, *t = strtok_r(list, "|", &save); t; t = strtok_r(NULL, "|", &save)) {
                char *colon = strchr(t, ':');
                if (!colon)
                    continue;
                *colon = 0;
                mf_action *a = mf_add(&s, t, colon + 1, !strcmp(t, "uninstall") || !strcmp(t, "move-to-trash") ? MF_DANGER : 0);
                if (a && !strcmp(t, "paste"))
                    a->flags |= MF_OFF, snprintf(a->why_off, sizeof a->why_off, "Nothing is copied");
            }
        }
        mf_add(&s, "ask-about-it", "Ask Mita about this", MF_QUIET);
        static const char *const glob[][2] = {{"take-screenshot", "Take a screenshot"}, {"open-terminal", "Open Terminal"},
                                              {"settings", "Settings"}};
        for (size_t g = 0; g < 3; g++)
            mf_add(&s, glob[g][0], glob[g][1], MF_GLOBAL);
        fm_node *m = mf_build(d, &s);
        int placed = 0, twice = 0;
        bool seen[MF_MAX] = {false};
        if (m)
            count_placed(m, seen, s.n, &placed, &twice);
        int offered = 0;
        for (int i = 0; i < s.n; i++)
            offered += !(s.a[i].flags & MF_OFF);
        printf("MENU %s: %s; %d of %d actions placed%s%s\n", mk, mf_template(d, mk) ? "the design's own" : "a ring",
               placed, offered, twice ? ", SOME TWICE" : "", placed < offered ? ", SOME MISSING" : "");
        if (m && getenv("MENUDUMP")) { /* what was made, as markup */
            char *t = fm_serialize_node(d, m);
            printf("%s", t ? t : "");
            free(t);
        }
        if (m) { /* kept in the text, open, for the pictures */
            m->generated = false;
            fm_attr_set(m, "id", "ctx-sample");
            free(doc);
            doc = fm_serialize(d);
            setenv("FOCUS", "#ctx-sample", 1);
        }
        fm_free(d);
    }
    stylist_look look;
    extern void stylist_look_focus(const char *sel);
    stylist_look_focus(getenv("FOCUS")); /* FOCUS="#start": a close-up of it, as a seeing model is shown */
    stylist_look_at(doc ? doc : buf, outdir != NULL, &look);
    if (!look.ok) {
        printf("DOES NOT HOLD TOGETHER: %s\n", look.problems);
        return 1;
    }
    if (look.problems[0])
        printf("reader's notes: %s\n", look.problems);
    printf("%d findings, score %d\n%s", look.lint.count, look.lint.score, look.lint.text);
    if (outdir) {
        mkdir(outdir, 0755);
        char p[512];
        snprintf(p, sizeof p, "%s/1-desktop.jpg", outdir);
        stylist_write_jpeg(p, look.jpeg_b64[0]);
        snprintf(p, sizeof p, "%s/2-open.jpg", outdir);
        stylist_write_jpeg(p, look.jpeg_b64[1]);
        printf("pictures: %s/1-desktop.jpg (%s), %s/2-open.jpg (%s)\n", outdir, look.shows[0], outdir, look.shows[1]);
        if (look.jpeg_b64[2]) {
            snprintf(p, sizeof p, "%s/3-closeup.jpg", outdir);
            stylist_write_jpeg(p, look.jpeg_b64[2]);
            printf("close-up: %s\n", p);
        }
    }
    return look.lint.score > 0;
}

/* The tray (volume, network, ...) needs the running system: a quiet dot
 * where each element goes, so layouts with a tray still look right. */
float fm_host_tray_extra(const fm_node *n) { (void)n; return 0; }
void fm_host_draw_tray(fm_node *n, rect r, color col, float op) {
    (void)n;
    float rad = (r.w < r.h ? r.w : r.h) * 0.22f;
    fill_circle(A.frame, r.x + r.w / 2.0f, r.y + r.h / 2.0f, rad, color_alpha(col, op * 0.8f));
}
