/* A Facet 2 app's view as a Facet document for the desktop's engine
 * (fxwin.c draws it; tests/fx_shot.c pictures it on the build machine).
 * token(...) becomes this design's values; inputs, fields and checkboxes
 * become boxes (fxin="N") the shell draws them into. */
#define _GNU_SOURCE
#include "markup.h"
#include "../../lib/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D (*dz)
static const design *dz;

typedef struct {
    fx_out **acts;
    int nacts;
} acts_t;

static void hex(char *out, size_t n, color c) { snprintf(out, n, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c)); }

/* token(name) in this design */
static void token_value(const char *name, char *out, size_t n) {
    color c;
    if (!strcmp(name, "accent")) c = D.accent;
    else if (!strcmp(name, "accent2")) c = D.accent2;
    else if (!strcmp(name, "background")) c = D.bg;
    else if (!strcmp(name, "surface")) c = D.surface;
    else if (!strcmp(name, "surface2")) c = color_mix(D.surface, D.surface_text, 0.07f);
    else if (!strcmp(name, "text")) c = D.surface_text;
    else if (!strcmp(name, "muted")) c = D.muted;
    else if (!strcmp(name, "danger")) c = color_mix(RGBA(214, 64, 64, 255), D.accent, 0.15f);
    else if (!strcmp(name, "success")) c = color_mix(RGBA(60, 170, 100, 255), D.accent, 0.15f);
    else if (!strcmp(name, "border")) c = color_mix(D.surface, D.surface_text, 0.2f);
    else if (!strcmp(name, "font-body")) { snprintf(out, n, "%s", D.font_body); return; }
    else if (!strcmp(name, "font-heading")) { snprintf(out, n, "%s", D.font_heading); return; }
    else if (!strcmp(name, "font-mono")) { snprintf(out, n, "JetBrainsMono"); return; }
    else c = D.surface_text;
    hex(out, n, c);
}

/* the app's rules in the engine's words: token(x) as this design's values, size/weight as font-size/-weight */
static void convert_style(sbuf *b, const char *style) {
    int depth = 0;
    for (const char *p = style; *p;) {
        if (*p == '{' || *p == '}') {
            depth += *p == '{' ? 1 : -1;
            sb_addn(b, p++, 1);
            continue;
        }
        if (!depth && *p == '#') { /* a selector's #id */
            sb_add(b, ".fx-id-");
            p++;
            continue;
        }
        if (!strncmp(p, "token(", 6)) {
            const char *e = strchr(p, ')');
            if (e) {
                char name[48], val[64];
                snprintf(name, sizeof name, "%.*s", (int)(e - p - 6) < 47 ? (int)(e - p - 6) : 47, p + 6);
                token_value(name, val, sizeof val);
                sb_add(b, val);
                p = e + 1;
                continue;
            }
        }
        /* property names at the start of a declaration */
        bool start = p == style || p[-1] == '{' || p[-1] == ';' || p[-1] == '\n' || p[-1] == ' ' || p[-1] == '\t';
        if (start && !strncmp(p, "size:", 5)) {
            sb_add(b, "font-size:");
            p += 5;
            continue;
        }
        if (start && !strncmp(p, "weight:", 7)) {
            sb_add(b, "font-weight:");
            p += 7;
            continue;
        }
        if (start && !strncmp(p, "font:", 5)) {
            sb_add(b, "font-family:");
            p += 5;
            continue;
        }
        sb_addn(b, p, 1);
        p++;
    }
}

static void esc(sbuf *b, const char *s) {
    for (; *s; s++)
        switch (*s) {
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '&': sb_add(b, "&amp;"); break;
        case '"': sb_add(b, "&quot;"); break;
        default: sb_addn(b, s, 1);
        }
}

static bool has_handler(const fx_out *o, const char *ev) {
    for (int i = 0; i < o->nh; i++)
        if (!strcmp(o->h[i].event, ev))
            return true;
    return false;
}

static void emit_markup(acts_t *fw, sbuf *b, fx_out *o) {
    if (!o->tag) {
        esc(b, o->text ? o->text : "");
        return;
    }
    const char *t = o->tag;
    bool input = !strcmp(t, "input"), fieldm = !strcmp(t, "field"), check = !strcmp(t, "checkbox");
    const char *tag = input || fieldm || check || !strcmp(t, "scroll") || !strcmp(t, "popover") || !strcmp(t, "sheet") ||
                              !strcmp(t, "image") || !strcmp(t, "canvas")
                          ? (!strcmp(t, "scroll") ? "column" : "panel")
                          : t;
    sb_printf(b, "<%s", tag);
    /* ids repeat in lists (one per item), so they go as a class (#x in its rules becomes .fx-id-x) */
    const char *id = fx_get(o, "id"), *cls = fx_get(o, "class");
    sb_add(b, " class=\"");
    if (cls)
        esc(b, cls);
    if (id) {
        sb_add(b, " fx-id-");
        esc(b, id);
    }
    if (input) sb_add(b, " fx-input");
    if (fieldm) sb_add(b, " fx-field");
    if (check) sb_add(b, " fx-check");
    if (!strcmp(t, "scroll")) sb_add(b, " fx-scroll");
    if (fx_get(o, "disabled")) sb_add(b, " fx-disabled");
    sb_add(b, "\"");
    if (input || fieldm || check) {
        sb_printf(b, " fxin=\"%d\"", fw->nacts);
    }
    if ((has_handler(o, "click") && !fx_get(o, "disabled")) || input || fieldm || check) {
        fw->acts = realloc(fw->acts, sizeof *fw->acts * (size_t)(fw->nacts + 1));
        fw->acts[fw->nacts] = o;
        if (!input && !fieldm && !check)
            sb_printf(b, " what=\"fx:%d\"", fw->nacts);
        fw->nacts++;
    }
    if (!strcmp(t, "icon") && fx_get(o, "name"))
        sb_printf(b, " name=\"%s\"", fx_get(o, "name"));
    sb_add(b, ">");
    for (int i = 0; i < o->nkids; i++)
        emit_markup(fw, b, o->kids[i]);
    sb_printf(b, "</%s>", tag);
}


char *fx_markup(fx_out *view, const fx_app *app, const design *d, fx_out ***acts, int *nacts) {
    dz = d;
    acts_t at = {NULL, 0};
    sbuf b;
    sb_init(&b);
    sb_add(&b, "<facet><desktop><column class=\"fx-root\" place=\"fill\" over=\"screen\" layer=\"below\">");
    for (int i = 0; i < view->nkids; i++)
        emit_markup(&at, &b, view->kids[i]);
    sb_add(&b, "</column></desktop><style>");
    /* the app's look starts from the person's design, then its own rules */
    char text[16], field[16], accent[16];
    hex(text, sizeof text, D.surface_text);
    hex(field, sizeof field, color_mix(D.surface, D.surface_text, 0.07f));
    hex(accent, sizeof accent, D.accent);
    float rad = D.radius * 0.5f < 4 ? 4 : D.radius * 0.5f;
    sb_printf(&b,
              ".fx-root { direction: column; gap: 8; padding: 14; color: %s; font-family: %s; font-size: %.0f; }\n"
              "row { direction: row; gap: 8; align: center; }\n column { direction: column; gap: 6; }\n"
              "button { padding: 7 14; radius: %.0f; background: %s; color: %s; transition: 120; }\n"
              "button:hover { background: %s; color: %s; }\n"
              ".fx-input { height: 38; min-width: 120; }\n .fx-field { min-height: 160; grow: 1; }\n"
              ".fx-check { width: 22; height: 22; }\n .fx-scroll { overflow: scroll; grow: 1; }\n"
              ".fx-disabled { opacity: 0.45; }\n",
              text, D.font_body, (double)D.body_px, (double)rad, field, text, accent, text);
    if (app->style)
        convert_style(&b, app->style);
    for (int i = 0; i < app->ncomps; i++)
        if (app->comps[i].style)
            convert_style(&b, app->comps[i].style);
    sb_add(&b, "</style></facet>");
    *acts = at.acts;
    *nacts = at.nacts;
    return b.p;
}
