/* Runs the Facet markup engine on the build machine: parses a document,
 * lays it out at 1600x900 and prints every element's box, plus problems.
 *   tests/fm-dump.sh tests/designs/orbit.xml [press #id ...]  */
#include "../userland/shell/fm/fm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ids[] = {"files", "companion", "notes", "settings", "about", "redesign", "power"};
int fm_host_app_count(void) { return 7; }
const char *fm_host_app_id(int a) { return a >= 0 && a < 7 ? ids[a] : ""; }
const char *fm_host_app_label(int a) { return fm_host_app_id(a); }
int fm_host_window_count(void) { return 1; }
int fm_host_window_app(int i) { (void)i; return 1; }
const char *fm_host_window_title(int i) { (void)i; return "Window"; }
bool fm_host_window_focused(int i) { return i == 0; }
void fm_host_action(fm_doc *d, fm_node *src, const char *action) { (void)d, (void)src; printf("ACTION %s\n", action); }
bool fm_host_bind(const char *key, char *out, size_t n) { snprintf(out, n, "<%s>", key); return true; }
void fm_host_draw_view(const char *app, rect r) { (void)app, (void)r; }
void fm_host_draw_orb(rect r, bool active) { (void)r, (void)active; }
const char *clipboard_text(void) { return ""; }
void fm_host_draw_icon(rect r, int app, const fm_style *st, bool hover) { (void)r, (void)app, (void)st, (void)hover; }
font *fm_host_font(const char *family, float px) { return font_get(family, px); }
color fm_host_wallpaper_tone(void) { return 0xff000000u; }
uint64_t fm_host_time_ms(void) { return 1234; }
color readable_on(color bg) { return color_luma(bg) > 0.5f ? 0xff000000u : 0xffffffffu; }
void icon_symbol(canvas *c, rect box, int app, float stroke, color col) { (void)c, (void)box, (void)app, (void)stroke, (void)col; }

static void dump(fm_node *n, int depth) {
    for (; n; n = n->next) {
        if (n->kind == FM_TEXT) {
            if (n->laid_out)
                printf("%*s\"%.40s\" box=%d,%d %dx%d\n", depth * 2, "", n->text, n->box.x, n->box.y, n->box.w, n->box.h);
            continue;
        }
        printf("%*s<%s#%s>%s%s box=%d,%d %dx%d vis=%d,%d %dx%d font=%s %.0f\n", depth * 2, "", n->tag, n->id,
               n->st.hidden ? " hidden" : "", n->open ? " open" : "", n->box.x, n->box.y, n->box.w, n->box.h, n->vis.x,
               n->vis.y, n->vis.w, n->vis.h, n->st.font, n->st.font_size);
        dump(n->first, depth + 1);
    }
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "r");
    if (!f)
        return 1;
    static char buf[1 << 20];
    buf[fread(buf, 1, sizeof buf - 1, f)] = 0;
    fclose(f);
    fonts_init(argc > 2 && !strncmp(argv[2], "--fonts=", 8) ? argv[2] + 8 : "build/ports/fonts");
    fm_doc *d = fm_parse(buf);
    for (int i = 2; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "press") || !strcmp(argv[i], "cmd"))
            printf("CMD %s -> %s\n", argv[i + 1], fm_command(d, argv[i + 1], NULL));
    canvas *c = canvas_new(1600, 900);
    for (int frame = 0; frame < 3; frame++) {
        fm_layout(d, (rect){0, 0, 1600, 900});
        fm_animate(d, 500);
        for (int l = 0; l < 3; l++)
            fm_render_layer(d, c, l);
    }
    rect content;
    fm_render_window_frame(d, c, 0, (rect){100, 100, 600, 400}, true, &content);
    printf("window content %d,%d %dx%d\n", content.x, content.y, content.w, content.h);
    printf("work area %d,%d %dx%d\n", d->work.x, d->work.y, d->work.w, d->work.h);
    for (int i = 0; i < d->nproblems; i++)
        printf("PROBLEM line %d%s: %s\n", d->problems[i].line, d->problems[i].fatal ? " (fatal)" : "", d->problems[i].msg);
    dump(d->root, 0);
    char *desc = fm_describe(d);
    printf("--- describe ---\n%s", desc);
    char *ser = fm_serialize(d);
    fm_doc *again = fm_parse(ser);
    printf("--- reparse problems: %d\n", again->nproblems);
    return 0;
}
